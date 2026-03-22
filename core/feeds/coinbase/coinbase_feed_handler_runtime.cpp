#include "coinbase_feed_handler.hpp"
#include "../common/feed_handler_utils.hpp"
#include <chrono>
#include <cstring>
#include <libwebsockets.h>
#include <vector>
#include <thread>

namespace trading {

namespace {

struct CoinbaseWsSession {
    CoinbaseFeedHandler* handler;
    struct lws* wsi{nullptr};
    bool established{false};
    bool closed{false};
    bool send_ping{false};
    int64_t last_ping_ns{0};
    size_t pending_msg_index{0};
    std::vector<std::string> subscribe_msgs;
    feed::FragmentBuffer fragments;
};

static auto coinbase_lws_cb(struct lws* wsi, enum lws_callback_reasons reason, void*,
                            void* input, size_t len) -> int {
    auto* session = static_cast<CoinbaseWsSession*>(lws_context_user(lws_get_context(wsi)));
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
        if (session->pending_msg_index < session->subscribe_msgs.size()) {
            const std::string& message = session->subscribe_msgs[session->pending_msg_index];
            std::vector<unsigned char> buffer(LWS_PRE + message.size());
            std::memcpy(buffer.data() + LWS_PRE, message.c_str(), message.size());
            lws_write(wsi, buffer.data() + LWS_PRE, message.size(), LWS_WRITE_TEXT);
            ++session->pending_msg_index;
            if (session->pending_msg_index < session->subscribe_msgs.size()) {
                lws_callback_on_writable(wsi);
            }
        } else if (session->send_ping) {
            unsigned char buffer[LWS_PRE + 4] = {};
            lws_write(wsi, buffer + LWS_PRE, 0, LWS_WRITE_PING);
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

static struct lws_protocols k_coinbase_protocols[] = {
    {"coinbase-l2", coinbase_lws_cb, 0, 131072, 0, nullptr, 0},
    {nullptr, nullptr, 0, 0, 0, nullptr, 0},
};

}

auto CoinbaseFeedHandler::start() -> Result {
    if (running_.load(std::memory_order_acquire)) {
        return Result::SUCCESS;
    }

    LOG_INFO("[Coinbase] Starting feed handler", "symbol", symbol_.c_str());

    const std::string api_key = coinbase_api_key_from_env();
    const std::string api_secret = coinbase_api_secret_from_env();
    if (api_key.empty() || api_secret.empty()) {
        LOG_WARN("[Coinbase] Starting unauthenticated level2 session", "symbol", symbol_.c_str());
    } else {
        const std::string test_jwt = generate_jwt(api_key, api_secret);
        if (test_jwt.empty()) {
            LOG_WARN("[Coinbase] JWT generation failed — falling back to unauthenticated level2 session",
                     "symbol", symbol_.c_str());
        }
    }

    fetch_tick_size();

    static constexpr int k_max_attempts = 3;
    static constexpr int k_backoff_ms[k_max_attempts] = {2000, 4000, 8000};

    for (int attempt = 1; attempt <= k_max_attempts; ++attempt) {
        running_.store(true, std::memory_order_release);
        state_.store(State::BUFFERING, std::memory_order_release);
        last_sequence_.store(0, std::memory_order_release);
        last_heartbeat_ns_.store(0, std::memory_order_release);
        last_event_time_exchange_ns_.store(0, std::memory_order_release);
        last_start_failure_reason_ = "timeout waiting for snapshot";
        delta_buffer_.clear();
        reconnect_requested_.store(false, std::memory_order_release);
        auth_rejected_.store(false, std::memory_order_release);
        subscription_rejected_.store(false, std::memory_order_release);

        ws_thread_ = std::thread([this]() -> void { ws_event_loop(); });

        std::unique_lock<std::mutex> lock(ws_mutex_);
        bool ready = ws_cv_.wait_for(lock, std::chrono::seconds(15), [this]() -> bool {
            return state_.load(std::memory_order_acquire) == State::STREAMING ||
                   !running_.load(std::memory_order_acquire);
        });
        lock.unlock();

        const bool streaming = state_.load(std::memory_order_acquire) == State::STREAMING;
        if (ready && streaming) {
            LOG_INFO("[Coinbase] feed handler started", "symbol", symbol_.c_str());
            return Result::SUCCESS;
        }

        const std::string sub_reason = last_start_failure_reason_;
        LOG_ERROR("[Coinbase] feed start failed", "symbol", symbol_.c_str(), "attempt", attempt,
                  "reason", sub_reason.c_str());

        running_.store(false, std::memory_order_release);
        if (auto* ctx =
                static_cast<struct lws_context*>(lws_ctx_.load(std::memory_order_acquire))) {
            lws_cancel_service(ctx);
        }
        ws_cv_.notify_all();
        if (ws_thread_.joinable()) {
            ws_thread_.join();
        }

        if (attempt < k_max_attempts) {
            LOG_INFO("[Coinbase] Retrying feed start", "symbol", symbol_.c_str(), "backoff_ms",
                     k_backoff_ms[attempt - 1]);
            std::this_thread::sleep_for(std::chrono::milliseconds(k_backoff_ms[attempt - 1]));
        }
    }

    LOG_ERROR("[Coinbase] feed permanently failed after all retries", "symbol", symbol_.c_str());
    emit_ops_event();
    return Result::ERROR_CONNECTION_LOST;
}

void CoinbaseFeedHandler::stop() {
    if (!running_.load(std::memory_order_acquire) && !ws_thread_.joinable()) {
        return;
    }

    LOG_INFO("[Coinbase] Stopping feed handler", "symbol", symbol_.c_str());
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

void CoinbaseFeedHandler::ws_event_loop() {
    constexpr uint32_t MAX_DELAY_MS = 30000;
    constexpr int64_t k_heartbeat_grace_ns = 5LL * 1'000'000'000LL;
    uint32_t delay_ms = 100;

    const auto endpoint =
        feed::parse_ws_endpoint(ws_url_, "advanced-trade-ws.coinbase.com", 443, "/");

    while (running_.load(std::memory_order_acquire)) {
        CoinbaseWsSession session;
        session.handler = this;
        session.subscribe_msgs = build_subscription_messages();

        lws_context_creation_info ctx_info = {};
        ctx_info.port = CONTEXT_PORT_NO_LISTEN;
        ctx_info.protocols = k_coinbase_protocols;
        ctx_info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
        ctx_info.user = &session;

        auto* ctx = lws_create_context(&ctx_info);
        if (ctx == nullptr) {
            last_start_failure_reason_ = "websocket context creation failed";
            LOG_ERROR("[Coinbase] lws_create_context failed", "symbol", symbol_.c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
            continue;
        }
        lws_ctx_.store(ctx, std::memory_order_release);

        lws_client_connect_info connect_info = {};
        connect_info.context = ctx;
        connect_info.address = endpoint.host.c_str();
        connect_info.port = endpoint.port;
        connect_info.path = endpoint.path.c_str();
        connect_info.host = endpoint.host.c_str();
        connect_info.origin = endpoint.host.c_str();
        connect_info.protocol = k_coinbase_protocols[0].name;
        connect_info.ssl_connection = LCCSCF_USE_SSL;

        if (lws_client_connect_via_info(&connect_info) == nullptr) {
            last_start_failure_reason_ = "websocket connect failed";
            LOG_ERROR("[Coinbase] WebSocket connect failed", "symbol", symbol_.c_str());
            lws_context_destroy(ctx);
            lws_ctx_.store(nullptr, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
            continue;
        }

        while (running_.load() && !session.established && !session.closed) {
            lws_service(ctx, 50);
        }

        if (!running_.load() || session.closed) {
            lws_context_destroy(ctx);
            lws_ctx_.store(nullptr, std::memory_order_release);
            if (running_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
            }
            continue;
        }

        LOG_INFO("[Coinbase] WebSocket established", "symbol", symbol_.c_str());
        delay_ms = 100;

        while (running_.load() && !session.closed) {
            if (reconnect_requested_.exchange(false, std::memory_order_acq_rel)) {
                session.closed = true;
                break;
            }
            const int64_t now_ns = http::now_ns();
            const int64_t heartbeat_ns = last_heartbeat_ns_.load(std::memory_order_acquire);
            if (heartbeat_ns > 0 && now_ns - heartbeat_ns > k_heartbeat_grace_ns) {
                last_start_failure_reason_ = "heartbeat timeout";
                trigger_resnapshot("Coinbase heartbeat timeout");
                session.closed = true;
                break;
            }
            if (now_ns - session.last_ping_ns >= 20LL * 1'000'000'000LL) {
                session.send_ping = true;
                session.last_ping_ns = now_ns;
                lws_callback_on_writable(session.wsi);
            }
            lws_service(ctx, 50);
        }

        lws_context_destroy(ctx);
        lws_ctx_.store(nullptr, std::memory_order_release);
        state_.store(State::BUFFERING, std::memory_order_release);

        if (running_.load(std::memory_order_acquire)) {
            LOG_WARN("[Coinbase] stream disconnected, reconnecting", "symbol", symbol_.c_str(),
                     "backoff_ms", delay_ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
        }
    }

    ws_cv_.notify_all();
}
