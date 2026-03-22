#include "binance_feed_handler.hpp"
#include "../common/feed_handler_utils.hpp"
#include "../common/tick_size.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <libwebsockets.h>
#include <thread>

namespace trading {

struct BinanceWsSession {
    BinanceFeedHandler* handler;
    struct lws* wsi{nullptr};
    bool established{false};
    bool closed{false};
    bool send_ping{false};
    int64_t last_ping_ns{0};
    feed::FragmentBuffer fragments;
};

static auto binance_lws_cb(struct lws* wsi, enum lws_callback_reasons reason, void*,
                           void* input, size_t len) -> int {
    auto* session = static_cast<BinanceWsSession*>(lws_context_user(lws_get_context(wsi)));
    if (session == nullptr) {
        return 0;
    }

    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        session->wsi = wsi;
        session->established = true;
        session->last_ping_ns = http::now_ns();
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

    case LWS_CALLBACK_CLIENT_WRITEABLE:
        if (session->send_ping) {
            unsigned char buf[LWS_PRE + 4] = {};
            lws_write(wsi, buf + LWS_PRE, 0, LWS_WRITE_PING);
            session->send_ping = false;
        }
        break;

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

static struct lws_protocols k_binance_protocols[] = {
    {"binance-depth", binance_lws_cb, 0, 131072, 0, nullptr, 0},
    {nullptr, nullptr, 0, 0, 0, nullptr, 0}};

BinanceFeedHandler::BinanceFeedHandler(const std::string& symbol, const std::string& api_url,
                                       const std::string& ws_url)
    : symbol_(symbol), api_url_(api_url), ws_url_(ws_url),
      venue_symbols_(SymbolMapper::map_all(symbol)) {
    LOG_INFO("[Binance] FeedHandler created", "symbol", symbol_.c_str());
}

BinanceFeedHandler::~BinanceFeedHandler() { stop(); }

auto BinanceFeedHandler::fetch_tick_size() -> Result {
    const std::string url = api_url_ + "/api/v3/exchangeInfo?symbol=" + venue_symbols_.binance;
    auto resp = http::get(url);
    if (!resp.ok() || resp.body.empty()) {
        LOG_WARN("[Binance] fetch_tick_size failed", "symbol", symbol_.c_str(), "status", resp.status);
        return Result::ERROR_CONNECTION_LOST;
    }
    auto json = nlohmann::json::parse(resp.body, nullptr, false);
    if (json.is_discarded()) {
        LOG_WARN("[Binance] fetch_tick_size JSON parse failed", "symbol", symbol_.c_str());
        return Result::ERROR_BOOK_CORRUPTED;
    }
    auto sym_it = json.find("symbols");
    if (sym_it == json.end() || !sym_it->is_array() || sym_it->empty()) {
        LOG_WARN("[Binance] fetch_tick_size: no symbols array", "symbol", symbol_.c_str());
        return Result::ERROR_BOOK_CORRUPTED;
    }
    for (const auto& f : (*sym_it)[0].value("filters", nlohmann::json::array())) {
        if (f.value("filterType", std::string("")) == "PRICE_FILTER") {
            std::string ts = f.value("tickSize", std::string(""));
            if (ts.empty()) {
                break;
            }
            tick_size_ = tick_from_string(ts);
            LOG_INFO("[Binance] Tick size fetched", "symbol", symbol_.c_str(), "tick_size", tick_size_);
            return Result::SUCCESS;
        }
    }
    LOG_WARN("[Binance] fetch_tick_size: PRICE_FILTER not found", "symbol", symbol_.c_str());
    return Result::ERROR_BOOK_CORRUPTED;
}

auto BinanceFeedHandler::sync_stats() const -> SyncStats {
    SyncStats stats;
    stats.resync_count = resync_count_;
    stats.snapshot_latency_ms = snapshot_latency_ms_;
    stats.buffered_applied = buffered_applied_;
    stats.buffered_skipped = buffered_skipped_;
    stats.buffer_high_water_mark = buffer_high_water_mark_;
    stats.last_resync_reason = last_resync_reason_;
    return stats;
}

auto BinanceFeedHandler::start() -> Result {
    if (running_.load(std::memory_order_acquire)) {
        return Result::SUCCESS;
    }

    LOG_INFO("[Binance] Starting feed handler", "symbol", symbol_.c_str());
    fetch_tick_size();
    reconnect_requested_.store(false, std::memory_order_release);
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

    LOG_INFO("[Binance] Feed handler started", "symbol", symbol_.c_str());
    return Result::SUCCESS;
}

void BinanceFeedHandler::stop() {
    if (!running_.load(std::memory_order_acquire) && !ws_thread_.joinable()) {
        return;
    }

    LOG_INFO("[Binance] Stopping feed handler", "symbol", symbol_.c_str());
    running_.store(false, std::memory_order_release);
    reconnect_requested_.store(false, std::memory_order_release);
    state_.store(State::DISCONNECTED, std::memory_order_release);

    if (auto* ctx = static_cast<struct lws_context*>(lws_ctx_.load(std::memory_order_acquire))) {
        lws_cancel_service(ctx);
    }
    ws_cv_.notify_all();

    if (ws_thread_.joinable()) {
        ws_thread_.join();
    }
}

void BinanceFeedHandler::ws_event_loop() {
    constexpr uint32_t MAX_DELAY_MS = 30000;
    uint32_t delay_ms = 100;

    std::string sym_lower = venue_symbols_.binance;
    for (auto& character : sym_lower) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    const std::string ws_path = "/ws/" + sym_lower + "@depth@100ms";

    const auto endpoint = feed::parse_ws_endpoint(ws_url_, "stream.binance.com", 9443, "/");

    BinanceWsSession session;
    session.handler = this;

    lws_context_creation_info ctx_info = {};
    ctx_info.port = CONTEXT_PORT_NO_LISTEN;
    ctx_info.protocols = k_binance_protocols;
    ctx_info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    ctx_info.user = &session;

    auto* ctx = lws_create_context(&ctx_info);
    if (ctx == nullptr) {
        LOG_ERROR("[Binance] lws_create_context failed", "symbol", symbol_.c_str());
        running_.store(false, std::memory_order_release);
        ws_cv_.notify_all();
        return;
    }
    lws_ctx_.store(ctx, std::memory_order_release);

    while (running_.load(std::memory_order_acquire)) {
        reconnect_requested_.store(false, std::memory_order_release);
        session.wsi = nullptr;
        session.established = false;
        session.closed = false;
        session.send_ping = false;
        session.last_ping_ns = 0;
        session.fragments.len = 0;

        lws_client_connect_info connect_info = {};
        connect_info.context = ctx;
        connect_info.address = endpoint.host.c_str();
        connect_info.port = endpoint.port;
        connect_info.path = ws_path.c_str();
        connect_info.host = endpoint.host.c_str();
        connect_info.origin = endpoint.host.c_str();
        connect_info.protocol = k_binance_protocols[0].name;
        connect_info.ssl_connection = LCCSCF_USE_SSL;

        if (lws_client_connect_via_info(&connect_info) == nullptr) {
            LOG_ERROR("[Binance] WebSocket connect failed", "symbol", symbol_.c_str());
            if (!running_.load(std::memory_order_acquire)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
            continue;
        }

        while (running_.load(std::memory_order_acquire) && !session.established && !session.closed) {
            lws_service(ctx, 50);
        }

        if (!running_.load(std::memory_order_acquire)) {
            break;
        }
        if (session.closed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
            continue;
        }

        LOG_INFO("[Binance] WebSocket established, fetching snapshot", "symbol", symbol_.c_str());
        state_.store(State::BUFFERING, std::memory_order_release);
        delta_buffer_.clear();

        bool snapshot_ready = false;
        while (running_.load(std::memory_order_acquire)) {
            if (process_snapshot() != Result::SUCCESS) {
                break;
            }
            if (delta_buffer_.empty()) {
                snapshot_ready = true;
                break;
            }

            const uint64_t snapshot_sequence = last_sequence_.load(std::memory_order_acquire);
            const uint64_t first_buffered_update_id = delta_buffer_.front().first_update_id;
            if (snapshot_sequence >= first_buffered_update_id) {
                snapshot_ready = true;
                break;
            }

            LOG_WARN("[Binance] Snapshot older than buffered stream head", "symbol", symbol_.c_str(),
                     "snapshot_sequence", snapshot_sequence, "first_buffered_U", first_buffered_update_id,
                     "buffer_size", delta_buffer_.size());
        }

        if (!running_.load(std::memory_order_acquire)) {
            break;
        }

        if (!snapshot_ready) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
            continue;
        }

        if (apply_buffered_deltas(last_sequence_.load(std::memory_order_acquire)) != Result::SUCCESS) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
            continue;
        }

        state_.store(State::STREAMING, std::memory_order_release);
        delay_ms = 100;
        ws_cv_.notify_all();

        while (running_.load(std::memory_order_acquire) && !session.closed) {
            lws_service(ctx, 1000);

            if (reconnect_requested_.load(std::memory_order_acquire)) {
                lws_cancel_service(ctx);
                break;
            }

            if (!session.closed && (session.wsi != nullptr)) {
                int64_t now = http::now_ns();
                if (now - session.last_ping_ns > 30'000'000'000LL) {
                    session.send_ping = true;
                    session.last_ping_ns = now;
                    lws_callback_on_writable(session.wsi);
                }
            }
        }

        reconnect_requested_.store(false, std::memory_order_release);

        if (running_.load(std::memory_order_acquire)) {
            LOG_WARN("[Binance] WebSocket disconnected, reconnecting", "symbol", symbol_.c_str(), "delay_ms",
                     delay_ms);
            if (state_.load(std::memory_order_acquire) == State::STREAMING) {
                trigger_resnapshot("WebSocket disconnected");
                reconnect_requested_.store(false, std::memory_order_release);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
        }
    }

    lws_context_destroy(ctx);
    lws_ctx_.store(nullptr, std::memory_order_release);
    ws_cv_.notify_all();
}

}
