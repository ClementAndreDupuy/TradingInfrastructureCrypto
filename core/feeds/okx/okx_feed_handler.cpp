#include "okx_feed_handler.hpp"
#include "../../common/rest_client.hpp"
#include "../common/feed_handler_utils.hpp"
#include "../common/tick_size.hpp"
#include <algorithm>
#include <chrono>
#include <libwebsockets.h>

namespace trading {

struct OkxWsSession {
    OkxFeedHandler* handler;
    struct lws* wsi{nullptr};
    bool established{false};
    bool closed{false};
    bool send_ping{false};
    bool send_subscribe{false};
    int64_t last_ping_ns{0};
    std::string subscribe_msg;
    feed::FragmentBuffer fragments;
};

static auto okx_lws_cb(struct lws* wsi, enum lws_callback_reasons reason, void* unused,
                       void* input, size_t len) -> int {
    auto* session = static_cast<OkxWsSession*>(lws_context_user(lws_get_context(wsi)));
    if (session == nullptr) {
        return 0;
    }

    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        session->wsi = wsi;
        session->established = true;
        session->send_subscribe = true;
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

static struct lws_protocols k_okx_protocols[] = {
    {"okx-books", okx_lws_cb, 0, 131072, 0, nullptr, 0},
    {nullptr, nullptr, 0, 0, 0, nullptr, 0},
};

OkxFeedHandler::OkxFeedHandler(const std::string& symbol, const std::string& api_url,
                               const std::string& ws_url)
    : symbol_(symbol), venue_symbols_(SymbolMapper::map_all(symbol)), instrument_id_(venue_symbols_.okx),
      api_url_(api_url), ws_url_(ws_url) {
    LOG_INFO("[OKX] FeedHandler created", "symbol", symbol_.c_str(), "inst_id", instrument_id_.c_str());
}

OkxFeedHandler::~OkxFeedHandler() { stop(); }

auto OkxFeedHandler::fetch_tick_size() -> Result {
    const std::string url =
        api_url_ + "/api/v5/public/instruments?instType=SPOT&instId=" + instrument_id_;
    auto resp = http::get(url);
    if (!resp.ok() || resp.body.empty()) {
        LOG_WARN("[OKX] fetch_tick_size failed", "symbol", symbol_.c_str(), "status", resp.status);
        return Result::ERROR_CONNECTION_LOST;
    }
    auto json = nlohmann::json::parse(resp.body, nullptr, false);
    if (json.is_discarded() || json.value("code", std::string("")) != "0") {
        LOG_WARN("[OKX] fetch_tick_size: bad response", "symbol", symbol_.c_str());
        return Result::ERROR_BOOK_CORRUPTED;
    }
    auto data_it = json.find("data");
    if (data_it == json.end() || !data_it->is_array() || data_it->empty()) {
        LOG_WARN("[OKX] fetch_tick_size: no data", "symbol", symbol_.c_str());
        return Result::ERROR_BOOK_CORRUPTED;
    }
    std::string ts = (*data_it)[0].value("tickSz", std::string(""));
    if (ts.empty()) {
        LOG_WARN("[OKX] fetch_tick_size: tickSz missing", "symbol", symbol_.c_str());
        return Result::ERROR_BOOK_CORRUPTED;
    }
    tick_size_ = tick_from_string(ts);
    LOG_INFO("[OKX] Tick size fetched", "symbol", symbol_.c_str(), "tick_size", tick_size_);
    return Result::SUCCESS;
}

auto OkxFeedHandler::start() -> Result {
    if (running_.load(std::memory_order_acquire)) {
        return Result::SUCCESS;
    }

    LOG_INFO("[OKX] Starting feed handler", "symbol", symbol_.c_str());
    fetch_tick_size();
    running_.store(true, std::memory_order_release);
    state_.store(State::BUFFERING, std::memory_order_release);

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

    LOG_INFO("[OKX] feed handler started", "symbol", symbol_.c_str());
    return Result::SUCCESS;
}

void OkxFeedHandler::stop() {
    if (!running_.load(std::memory_order_acquire) && !ws_thread_.joinable()) {
        return;
    }

    LOG_INFO("[OKX] Stopping feed handler", "symbol", symbol_.c_str());
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

void OkxFeedHandler::ws_event_loop() {
    constexpr uint32_t MAX_DELAY_MS = 30000;
    uint32_t delay_ms = 100;

    const auto endpoint = feed::parse_ws_endpoint(ws_url_, "ws.okx.com", 8443, "/ws/v5/public");

    OkxWsSession session;
    session.handler = this;

    nlohmann::json sub = {
        {"op", "subscribe"},
        {"args", nlohmann::json::array({{{"channel", "books"}, {"instId", instrument_id_}}})},
    };
    session.subscribe_msg = sub.dump();

    lws_context_creation_info ctx_info = {};
    ctx_info.port = CONTEXT_PORT_NO_LISTEN;
    ctx_info.protocols = k_okx_protocols;
    ctx_info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    ctx_info.user = &session;

    auto* ctx = lws_create_context(&ctx_info);
    if (ctx == nullptr) {
        LOG_ERROR("[OKX] lws_create_context failed", "symbol", symbol_.c_str());
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
        session.send_subscribe = false;
        session.last_ping_ns = 0;
        session.fragments.len = 0;

        lws_client_connect_info connect_info = {};
        connect_info.context = ctx;
        connect_info.address = endpoint.host.c_str();
        connect_info.port = endpoint.port;
        connect_info.path = endpoint.path.c_str();
        connect_info.host = endpoint.host.c_str();
        connect_info.origin = endpoint.host.c_str();
        connect_info.protocol = k_okx_protocols[0].name;
        connect_info.ssl_connection = LCCSCF_USE_SSL;

        if (lws_client_connect_via_info(&connect_info) == nullptr) {
            LOG_ERROR("[OKX] WebSocket connect failed", "symbol", symbol_.c_str());
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

        LOG_INFO("[OKX] WebSocket established", "symbol", symbol_.c_str());

        state_.store(State::BUFFERING, std::memory_order_release);
        delta_buffer_.clear();
        bids_.clear();
        asks_.clear();
        last_sequence_.store(0, std::memory_order_release);
        force_reconnect_.store(false, std::memory_order_release);
        delay_ms = 100;

        while (running_.load() && !session.closed && !force_reconnect_.load()) {
            lws_service(ctx, 1000);

            if (!session.closed && (session.wsi != nullptr)) {
                int64_t now_ns = http::now_ns();
                if (now_ns - session.last_ping_ns >= 20LL * 1'000'000'000LL) {
                    session.send_ping = true;
                    session.last_ping_ns = now_ns;
                    lws_callback_on_writable(session.wsi);
                }
            }
        }

        force_reconnect_.store(false, std::memory_order_release);

        if (running_.load(std::memory_order_acquire)) {
            LOG_WARN("[OKX] stream disconnected, reconnecting", "symbol", symbol_.c_str(),
                     "backoff_ms", delay_ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
        }
    }

    lws_context_destroy(ctx);
    lws_ctx_.store(nullptr, std::memory_order_release);
    ws_cv_.notify_all();
}

}
