#include "kraken_feed_handler.hpp"
#include "../../common/rest_client.hpp"
#include "../common/feed_handler_utils.hpp"
#include "../common/tick_size.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <libwebsockets.h>

namespace trading {

struct KrakenWsSession {
    KrakenFeedHandler* handler;
    struct lws* wsi{nullptr};
    bool established{false};
    bool closed{false};
    bool send_ping{false};
    bool send_subscribe{false};
    int64_t last_ping_ns{0};
    std::string subscribe_msg;
    feed::FragmentBuffer fragments;
};

static auto kraken_lws_cb(struct lws* wsi, enum lws_callback_reasons reason, void*,
                          void* input, size_t len) -> int {
    auto* session = static_cast<KrakenWsSession*>(lws_context_user(lws_get_context(wsi)));
    if (session == nullptr) {
        return 0;
    }

    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        session->wsi = wsi;
        session->established = true;
        session->last_ping_ns = http::now_ns();
        lws_callback_on_writable(wsi);
        lws_cancel_service(lws_get_context(wsi));
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE: {
        const std::string message =
            feed::append_ws_fragment(session->fragments, static_cast<const char*>(input), len,
                                     lws_is_final_fragment(wsi) != 0);
        if (!message.empty() && session->handler != nullptr) {
            session->handler->process_message(message);
        }
        break;
    }

    case LWS_CALLBACK_CLIENT_WRITEABLE: {
        if (session->send_subscribe && !session->subscribe_msg.empty()) {
            std::vector<unsigned char> buf(LWS_PRE + session->subscribe_msg.size());
            std::memcpy(buf.data() + LWS_PRE, session->subscribe_msg.c_str(),
                        session->subscribe_msg.size());
            lws_write(wsi, buf.data() + LWS_PRE, session->subscribe_msg.size(), LWS_WRITE_TEXT);
            session->send_subscribe = false;
        } else if (session->send_ping) {
            unsigned char buf[LWS_PRE + 4] = {};
            lws_write(wsi, buf + LWS_PRE, 0, LWS_WRITE_PING);
            session->send_ping = false;
        }
        break;
    }

    case LWS_CALLBACK_CLIENT_CLOSED:
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        session->closed = true;
        lws_cancel_service(lws_get_context(wsi));
        break;

    default:
        break;
    }
    return 0;
}

static struct lws_protocols k_kraken_protocols[] = {
    {"kraken-book", kraken_lws_cb, 0, 131072, 0, nullptr, 0},
    {nullptr, nullptr, 0, 0, 0, nullptr, 0}};

KrakenFeedHandler::KrakenFeedHandler(const std::string& symbol, const std::string& api_url,
                                     const std::string& ws_url)
    : symbol_(symbol), api_url_(api_url), ws_url_(ws_url),
      venue_symbols_(SymbolMapper::map_all(symbol)) {
    LOG_INFO("[Kraken] FeedHandler created", "symbol", symbol_.c_str());
}

KrakenFeedHandler::~KrakenFeedHandler() { stop(); }

auto KrakenFeedHandler::fetch_tick_size() -> Result {
    const std::string url = api_url_ + "/0/public/AssetPairs?pair=" + venue_symbols_.kraken_rest;
    auto resp = http::get(url);
    if (resp.body.empty()) {
        LOG_WARN("[Kraken] fetch_tick_size failed", "symbol", symbol_.c_str(), "status", resp.status);
        return Result::ERROR_CONNECTION_LOST;
    }
    auto json = nlohmann::json::parse(resp.body, nullptr, false);
    if (json.is_discarded()) {
        LOG_WARN("[Kraken] fetch_tick_size JSON parse failed", "symbol", symbol_.c_str());
        return Result::ERROR_BOOK_CORRUPTED;
    }
    auto err_it = json.find("error");
    if (err_it != json.end() && err_it->is_array() && !err_it->empty()) {
        LOG_WARN("[Kraken] fetch_tick_size: API error", "symbol", symbol_.c_str());
        return Result::ERROR_BOOK_CORRUPTED;
    }
    auto res_it = json.find("result");
    if (res_it == json.end() || !res_it->is_object() || res_it->empty()) {
        LOG_WARN("[Kraken] fetch_tick_size: no result", "symbol", symbol_.c_str());
        return Result::ERROR_BOOK_CORRUPTED;
    }
    const auto& pair_data = res_it->begin().value();
    auto ts_it = pair_data.find("tick_size");
    if (ts_it != pair_data.end() && ts_it->is_string()) {
        std::string ts = ts_it->get<std::string>();
        if (!ts.empty()) {
            tick_size_ = tick_from_string(ts);
            LOG_INFO("[Kraken] Tick size fetched", "symbol", symbol_.c_str(), "tick_size", tick_size_);
            return Result::SUCCESS;
        }
    }
    auto dec_it = pair_data.find("pair_decimals");
    if (dec_it == pair_data.end() || !dec_it->is_number_integer()) {
        LOG_WARN("[Kraken] fetch_tick_size: tick_size and pair_decimals both missing", "symbol",
                 symbol_.c_str());
        return Result::ERROR_BOOK_CORRUPTED;
    }
    tick_size_ = std::pow(10.0, -dec_it->get<int>());
    LOG_INFO("[Kraken] Tick size fetched", "symbol", symbol_.c_str(), "tick_size", tick_size_);
    return Result::SUCCESS;
}

auto KrakenFeedHandler::start() -> Result {
    if (running_.load(std::memory_order_acquire)) {
        return Result::SUCCESS;
    }

    LOG_INFO("[Kraken] Starting feed handler", "symbol", symbol_.c_str());
    fetch_tick_size();
    running_.store(true, std::memory_order_release);
    state_.store(State::BUFFERING, std::memory_order_release);
    last_sequence_.store(0, std::memory_order_release);
    bids_.clear();
    asks_.clear();
    delta_buffer_.clear();

    ws_thread_ = std::thread([this]() -> void { ws_event_loop(); });

    std::unique_lock<std::mutex> lock(ws_mutex_);
    bool ready = ws_cv_.wait_for(lock, std::chrono::seconds(30), [this]() -> bool {
        return state_.load(std::memory_order_acquire) == State::STREAMING ||
               !running_.load(std::memory_order_acquire);
    });

    if (!ready || !running_.load(std::memory_order_acquire)) {
        running_.store(false, std::memory_order_release);
        if (auto* ctx =
                static_cast<struct lws_context*>(lws_ctx_.load(std::memory_order_acquire))) {
            lws_cancel_service(ctx);
        }
        ws_cv_.notify_all();
        if (ws_thread_.joinable()) {
            ws_thread_.join();
        }
        return Result::ERROR_CONNECTION_LOST;
    }

    LOG_INFO("[Kraken] Feed handler started", "symbol", symbol_.c_str());
    return Result::SUCCESS;
}

void KrakenFeedHandler::stop() {
    if (!running_.load(std::memory_order_acquire) && !ws_thread_.joinable()) {
        return;
    }

    LOG_INFO("[Kraken] Stopping feed handler", "symbol", symbol_.c_str());
    running_.store(false, std::memory_order_release);
    state_.store(State::DISCONNECTED, std::memory_order_release);

    if (auto* ctx = static_cast<struct lws_context*>(lws_ctx_.load(std::memory_order_acquire))) {
        lws_cancel_service(ctx);
    }
    ws_cv_.notify_all();

    if (ws_thread_.joinable()) {
        ws_thread_.join();
    }
}

void KrakenFeedHandler::ws_event_loop() {
    constexpr uint32_t MAX_DELAY_MS = 30000;
    uint32_t delay_ms = 100;

    const auto endpoint = feed::parse_ws_endpoint(ws_url_, "ws.kraken.com", 443, "/v2");

    const std::string ws_sym = venue_symbols_.kraken_ws;
    const std::string subscribe_msg =
        R"({"method":"subscribe","params":{"channel":"book","symbol":[")" + ws_sym +
        R"("],"depth":100,"snapshot":true}})";

    KrakenWsSession session;
    session.handler = this;

    lws_context_creation_info ctx_info = {};
    ctx_info.port = CONTEXT_PORT_NO_LISTEN;
    ctx_info.protocols = k_kraken_protocols;
    ctx_info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    ctx_info.user = &session;

    auto* ctx = lws_create_context(&ctx_info);
    if (ctx == nullptr) {
        LOG_ERROR("[Kraken] lws_create_context failed", "symbol", symbol_.c_str());
        running_.store(false, std::memory_order_release);
        ws_cv_.notify_all();
        return;
    }
    lws_ctx_.store(ctx, std::memory_order_release);

    while (running_.load(std::memory_order_acquire)) {
        session.wsi = nullptr;
        session.established = false;
        session.closed = false;
        session.send_ping = false;
        session.send_subscribe = true;
        session.last_ping_ns = 0;
        session.subscribe_msg = subscribe_msg;
        session.fragments.len = 0;

        lws_client_connect_info connect_info = {};
        connect_info.context = ctx;
        connect_info.address = endpoint.host.c_str();
        connect_info.port = endpoint.port;
        connect_info.path = endpoint.path.c_str();
        connect_info.host = endpoint.host.c_str();
        connect_info.origin = endpoint.host.c_str();
        connect_info.protocol = k_kraken_protocols[0].name;
        connect_info.ssl_connection = LCCSCF_USE_SSL;

        if (lws_client_connect_via_info(&connect_info) == nullptr) {
            LOG_ERROR("[Kraken] WebSocket connect failed", "symbol", symbol_.c_str());
            if (!running_.load(std::memory_order_acquire)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
            continue;
        }

        while (running_.load() && !session.established && !session.closed) {
            lws_service(ctx, 50);
        }

        if (!running_.load()) {
            break;
        }
        if (session.closed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
            continue;
        }

        LOG_INFO("[Kraken] WebSocket established, waiting for snapshot", "symbol", symbol_.c_str());
        state_.store(State::BUFFERING, std::memory_order_release);
        delta_buffer_.clear();
        bids_.clear();
        asks_.clear();
        last_sequence_.store(0, std::memory_order_release);
        force_reconnect_.store(false, std::memory_order_release);
        delay_ms = 100;

        while (running_.load() && !session.closed && !force_reconnect_.load(std::memory_order_acquire)) {
            lws_service(ctx, 1000);

            if (!session.closed && (session.wsi != nullptr)) {
                int64_t now = http::now_ns();
                if (now - session.last_ping_ns > 20'000'000'000LL) {
                    session.send_ping = true;
                    session.last_ping_ns = now;
                    lws_callback_on_writable(session.wsi);
                }
            }
        }

        force_reconnect_.store(false, std::memory_order_release);

        if (running_.load(std::memory_order_acquire)) {
            LOG_WARN("[Kraken] WebSocket disconnected, reconnecting", "symbol", symbol_.c_str(),
                     "delay_ms", delay_ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
        }
    }

    lws_context_destroy(ctx);
    lws_ctx_.store(nullptr, std::memory_order_release);
    ws_cv_.notify_all();
}

}
