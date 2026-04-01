#include "binance_futures_feed_handler.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <libwebsockets.h>
#include <nlohmann/json.hpp>
#include <thread>

namespace trading {
    struct BinanceFuturesWsSession {
        BinanceFuturesFeedHandler *handler;
        struct lws *wsi{nullptr};
        bool established{false};
        bool closed{false};
        bool send_ping{false};
        int64_t last_ping_ns{0};
        char frag_buf[65536];
        size_t frag_len{0};
    };

    static auto binance_futures_lws_cb(struct lws *wsi, enum lws_callback_reasons reason, void *,
                                       void *input, size_t len) -> int {
        auto *session =
            static_cast<BinanceFuturesWsSession *>(lws_context_user(lws_get_context(wsi)));
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
                const char *data = static_cast<const char *>(input);
                bool is_final = lws_is_final_fragment(wsi) != 0;
                if (session->frag_len + len <= sizeof(session->frag_buf)) {
                    std::memcpy(session->frag_buf + session->frag_len, data, len);
                    session->frag_len += len;
                } else {
                    session->frag_len = 0;
                }
                if (is_final && session->frag_len > 0 && session->handler != nullptr) {
                    const std::string msg(session->frag_buf, session->frag_len);
                    session->frag_len = 0;

                    auto json = nlohmann::json::parse(msg, nullptr, false);
                    if (!json.is_discarded()) {
                        auto p_it = json.find("p");
                        if (p_it != json.end() && p_it->is_string()) {
                            try {
                                const double price = std::stod(p_it->get<std::string>());
                                if (price > 0.0) {
                                    session->handler->dispatch_mark_price(price);
                                }
                            } catch (...) {
                            }
                        }
                    }
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

    static struct lws_protocols k_binance_futures_protocols[] = {
        {"binance-futures-mark", binance_futures_lws_cb, 0, 65536, 0, nullptr, 0},
        {nullptr, nullptr, 0, 0, 0, nullptr, 0}
    };

    BinanceFuturesFeedHandler::BinanceFuturesFeedHandler(const std::string &symbol,
                                                         const std::string &ws_url)
        : symbol_(symbol), ws_url_(ws_url) {
        futures_symbol_ = SymbolMapper::map_for_binance_usdm_futures(symbol);
        std::transform(futures_symbol_.begin(), futures_symbol_.end(), futures_symbol_.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        LOG_INFO("[BinanceFutures] FeedHandler created", "symbol", symbol_.c_str(),
                 "futures_symbol", futures_symbol_.c_str());
    }

    BinanceFuturesFeedHandler::~BinanceFuturesFeedHandler() { stop(); }

    void BinanceFuturesFeedHandler::start() {
        if (running_.load(std::memory_order_acquire)) {
            return;
        }
        running_.store(true, std::memory_order_release);
        ws_thread_ = std::thread([this]() -> void { ws_event_loop(); });
    }

    void BinanceFuturesFeedHandler::stop() {
        if (!running_.load(std::memory_order_acquire) && !ws_thread_.joinable()) {
            return;
        }
        running_.store(false, std::memory_order_release);
        if (auto *ctx =
                static_cast<struct lws_context *>(lws_ctx_.load(std::memory_order_acquire))) {
            lws_cancel_service(ctx);
        }
        ws_cv_.notify_all();
        if (ws_thread_.joinable()) {
            ws_thread_.join();
        }
    }

    void BinanceFuturesFeedHandler::ws_event_loop() {
        constexpr uint32_t MAX_DELAY_MS = 30000;
        uint32_t delay_ms = 100;

        const std::string ws_path = "/ws/" + futures_symbol_ + "@markPrice";

        std::string ws_host = "fstream.binance.com";
        int ws_port = 443;
        {
            size_t pos = ws_url_.find("://");
            if (pos != std::string::npos) {
                pos += 3;
            } else {
                pos = 0;
            }
            size_t slash = ws_url_.find('/', pos);
            std::string authority =
                ws_url_.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
            size_t colon = authority.rfind(':');
            if (colon != std::string::npos) {
                ws_host = authority.substr(0, colon);
                ws_port = std::stoi(authority.substr(colon + 1));
            } else if (!authority.empty()) {
                ws_host = authority;
            }
        }

        BinanceFuturesWsSession session;
        session.handler = this;

        lws_context_creation_info ctx_info = {};
        ctx_info.port = CONTEXT_PORT_NO_LISTEN;
        ctx_info.protocols = k_binance_futures_protocols;
        ctx_info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
        ctx_info.user = &session;

        auto *ctx = lws_create_context(&ctx_info);
        if (ctx == nullptr) {
            LOG_ERROR("[BinanceFutures] lws_create_context failed", "symbol", symbol_.c_str());
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
            session.last_ping_ns = 0;
            session.frag_len = 0;

            lws_client_connect_info connect_info = {};
            connect_info.context = ctx;
            connect_info.address = ws_host.c_str();
            connect_info.port = ws_port;
            connect_info.path = ws_path.c_str();
            connect_info.host = ws_host.c_str();
            connect_info.origin = ws_host.c_str();
            connect_info.protocol = k_binance_futures_protocols[0].name;
            connect_info.ssl_connection = LCCSCF_USE_SSL;

            if (lws_client_connect_via_info(&connect_info) == nullptr) {
                LOG_ERROR("[BinanceFutures] WebSocket connect failed", "symbol", symbol_.c_str());
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

            LOG_INFO("[BinanceFutures] WebSocket established", "symbol", symbol_.c_str(),
                     "stream", ws_path.c_str());
            delay_ms = 100;

            while (running_.load() && !session.closed) {
                lws_service(ctx, 1000);

                if (!session.closed && session.wsi != nullptr) {
                    int64_t now_ns = http::now_ns();
                    if (now_ns - session.last_ping_ns >= 20LL * 1'000'000'000LL) {
                        session.send_ping = true;
                        session.last_ping_ns = now_ns;
                        lws_callback_on_writable(session.wsi);
                    }
                }
            }

            if (running_.load(std::memory_order_acquire)) {
                LOG_WARN("[BinanceFutures] stream disconnected, reconnecting", "symbol",
                         symbol_.c_str(), "backoff_ms", delay_ms);
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
            }
        }

        lws_context_destroy(ctx);
        lws_ctx_.store(nullptr, std::memory_order_release);
        ws_cv_.notify_all();
    }
}
