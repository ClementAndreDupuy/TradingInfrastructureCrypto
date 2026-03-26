#include "coinbase_feed_handler.hpp"
#include "../../common/rest_client.hpp"
#include "../common/tick_size.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <ctime>
#include <exception>
#include <iomanip>
#include <libwebsockets.h>
#include <random>
#include <sstream>
#include <openssl/bn.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>

namespace trading {
    namespace {
        struct CoinbaseWsSession {
            CoinbaseFeedHandler *handler;
            struct lws *wsi{nullptr};
            bool established{false};
            bool closed{false};
            bool send_ping{false};
            int64_t last_ping_ns{0};
            size_t pending_msg_index{0};
            std::vector<std::string> subscribe_msgs;
            char frag_buf[131072];
            size_t frag_len{0};
        };

        static auto base64url_encode(const unsigned char *data, size_t len) -> std::string {
            static constexpr char k_chars[] =
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
            std::string out;
            out.reserve(((len + 2U) / 3U) * 4U);
            for (size_t i = 0; i < len; i += 3U) {
                const unsigned int octet_a = data[i];
                const unsigned int octet_b = (i + 1U < len) ? data[i + 1U] : 0U;
                const unsigned int octet_c = (i + 2U < len) ? data[i + 2U] : 0U;
                const unsigned int triple = (octet_a << 16U) | (octet_b << 8U) | octet_c;
                out.push_back(k_chars[(triple >> 18U) & 0x3FU]);
                out.push_back(k_chars[(triple >> 12U) & 0x3FU]);
                if (i + 1U < len) {
                    out.push_back(k_chars[(triple >> 6U) & 0x3FU]);
                }
                if (i + 2U < len) {
                    out.push_back(k_chars[triple & 0x3FU]);
                }
            }
            return out;
        }

        static auto base64url_encode(const std::string &data) -> std::string {
            return base64url_encode(reinterpret_cast<const unsigned char *>(data.data()), data.size());
        }

        static auto random_hex(size_t bytes) -> std::string {
            static constexpr char k_hex[] = "0123456789abcdef";
            std::string out(bytes * 2U, '0');
            std::vector<unsigned char> buf(bytes);
            if (RAND_bytes(buf.data(), static_cast<int>(buf.size())) == 1) {
                for (size_t i = 0; i < bytes; ++i) {
                    out[2U * i] = k_hex[(buf[i] >> 4U) & 0xFU];
                    out[2U * i + 1U] = k_hex[buf[i] & 0xFU];
                }
                return out;
            }
            std::random_device rd;
            for (size_t i = 0; i < bytes; ++i) {
                const unsigned char b = static_cast<unsigned char>(rd());
                out[2U * i] = k_hex[(b >> 4U) & 0xFU];
                out[2U * i + 1U] = k_hex[b & 0xFU];
            }
            return out;
        }

        static auto ecdsa_der_to_jose(const unsigned char *der, size_t der_len) -> std::string {
            const unsigned char *cursor = der;
            ECDSA_SIG *sig = d2i_ECDSA_SIG(nullptr, &cursor, static_cast<long>(der_len));
            if (sig == nullptr) {
                return std::string();
            }

            const BIGNUM *r = nullptr;
            const BIGNUM *s = nullptr;
            ECDSA_SIG_get0(sig, &r, &s);
            if (r == nullptr || s == nullptr) {
                ECDSA_SIG_free(sig);
                return std::string();
            }

            std::array<unsigned char, 64> raw = {};
            if (BN_bn2binpad(r, raw.data(), 32) != 32 || BN_bn2binpad(s, raw.data() + 32, 32) != 32) {
                ECDSA_SIG_free(sig);
                return std::string();
            }

            ECDSA_SIG_free(sig);
            return base64url_encode(raw.data(), raw.size());
        }

        static auto parse_rfc3339_ns(const std::string &value) -> int64_t {
            if (value.size() < 20U) {
                return 0;
            }
            std::tm tm = {};
            std::istringstream ss(value.substr(0, 19));
            ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
            if (ss.fail()) {
                return 0;
            }
            int64_t nanos = 0;
            size_t pos = 19U;
            if (pos < value.size() && value[pos] == '.') {
                ++pos;
                size_t digits = 0;
                while (pos < value.size() && std::isdigit(static_cast<unsigned char>(value[pos])) &&
                       digits < 9U) {
                    nanos = nanos * 10 + static_cast<int64_t>(value[pos] - '0');
                    ++pos;
                    ++digits;
                }
                while (digits < 9U) {
                    nanos *= 10;
                    ++digits;
                }
                while (pos < value.size() && std::isdigit(static_cast<unsigned char>(value[pos]))) {
                    ++pos;
                }
            }
            time_t seconds = timegm(&tm);
            if (seconds < 0) {
                return 0;
            }
            return static_cast<int64_t>(seconds) * 1'000'000'000LL + nanos;
        }

        static auto coinbase_lws_cb(struct lws *wsi, enum lws_callback_reasons reason, [[maybe_unused]] void *unused,
                                    void *input, size_t len) -> int {
            auto *session = static_cast<CoinbaseWsSession *>(lws_context_user(lws_get_context(wsi)));
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
                    const char *data = static_cast<const char *>(input);
                    bool is_final = lws_is_final_fragment(wsi) != 0;
                    if (session->frag_len + len <= sizeof(session->frag_buf)) {
                        std::memcpy(session->frag_buf + session->frag_len, data, len);
                        session->frag_len += len;
                    } else {
                        session->frag_len = 0;
                    }
                    if (is_final && session->frag_len > 0 && (session->handler != nullptr)) {
                        session->handler->process_message(std::string(session->frag_buf, session->frag_len));
                        session->frag_len = 0;
                    }
                    break;
                }

                case LWS_CALLBACK_CLIENT_WRITEABLE: {
                    if (session->pending_msg_index < session->subscribe_msgs.size()) {
                        const std::string &msg = session->subscribe_msgs[session->pending_msg_index];
                        std::vector<unsigned char> buf(LWS_PRE + msg.size());
                        std::memcpy(buf.data() + LWS_PRE, msg.c_str(), msg.size());
                        lws_write(wsi, buf.data() + LWS_PRE, msg.size(), LWS_WRITE_TEXT);
                        ++session->pending_msg_index;
                        if (session->pending_msg_index < session->subscribe_msgs.size()) {
                            lws_callback_on_writable(wsi);
                        }
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

        static struct lws_protocols k_coinbase_protocols[] = {
            {"coinbase-l2", coinbase_lws_cb, 0, 131072, 0, nullptr, 0},
            {nullptr, nullptr, 0, 0, 0, nullptr, 0},
        };
    }

    CoinbaseFeedHandler::CoinbaseFeedHandler(const std::string &symbol, const std::string &ws_url,
                                             const std::string &api_url)
        : symbol_(symbol), ws_url_(ws_url), api_url_(api_url),
          venue_symbols_(SymbolMapper::map_all(symbol)) {
        LOG_INFO("[Coinbase] FeedHandler created", "symbol", symbol_.c_str());
    }

    CoinbaseFeedHandler::~CoinbaseFeedHandler() { stop(); }

    auto CoinbaseFeedHandler::coinbase_api_key_from_env() -> std::string {
        std::string value = http::env_var("COINBASE_API_KEY");
        if (!value.empty()) {
            return value;
        }
        value = http::env_var("LIVE_COINBASE_API_KEY");
        if (!value.empty()) {
            return value;
        }
        return http::env_var("SHADOW_COINBASE_API_KEY");
    }

    auto CoinbaseFeedHandler::coinbase_api_secret_from_env() -> std::string {
        std::string value = http::env_var("COINBASE_API_SECRET");
        if (!value.empty()) {
            return value;
        }
        value = http::env_var("LIVE_COINBASE_API_SECRET");
        if (!value.empty()) {
            return value;
        }
        return http::env_var("SHADOW_COINBASE_API_SECRET");
    }

    auto CoinbaseFeedHandler::generate_jwt(const std::string &api_key, const std::string &api_secret)
        -> std::string {
        if (api_key.empty() || api_secret.empty()) {
            return std::string();
        }

        const int64_t now_s = http::now_ns() / 1'000'000'000LL;
        nlohmann::json header = {{"typ", "JWT"}, {"alg", "ES256"}, {"kid", api_key}, {"nonce", random_hex(16)}};
        nlohmann::json payload = {
            {"iss", "cdp"},
            {"nbf", now_s},
            {"exp", now_s + 120},
            {"sub", api_key},
        };

        const std::string signing_input = base64url_encode(header.dump()) + "." +
                                          base64url_encode(payload.dump());
        std::string pem_secret = api_secret;
        {
            size_t pos = 0;
            while ((pos = pem_secret.find("\\n", pos)) != std::string::npos) {
                pem_secret.replace(pos, 2, "\n");
                ++pos;
            }
        }

        BIO *bio = BIO_new_mem_buf(pem_secret.data(), static_cast<int>(pem_secret.size()));
        if (bio == nullptr) {
            return std::string();
        }
        EVP_PKEY *key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (key == nullptr) {
            return std::string();
        }

        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        if (ctx == nullptr) {
            EVP_PKEY_free(key);
            return std::string();
        }

        size_t der_len = 0;
        std::string jwt;
        if (EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, key) == 1 &&
            EVP_DigestSignUpdate(ctx, signing_input.data(), signing_input.size()) == 1 &&
            EVP_DigestSignFinal(ctx, nullptr, &der_len) == 1) {
            std::vector<unsigned char> der(der_len);
            if (EVP_DigestSignFinal(ctx, der.data(), &der_len) == 1) {
                der.resize(der_len);
                const std::string signature = ecdsa_der_to_jose(der.data(), der.size());
                if (!signature.empty()) {
                    jwt = signing_input + "." + signature;
                }
            }
        }

        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(key);
        return jwt;
    }

    auto CoinbaseFeedHandler::build_subscription_messages() -> std::vector<std::string> {
        std::vector<std::string> messages;
        const std::string api_key = coinbase_api_key_from_env();
        const std::string api_secret = coinbase_api_secret_from_env();
        const bool have_credentials = !api_key.empty() && !api_secret.empty();

        auto make_sub = [&](const char *channel, bool include_product_ids) {
            nlohmann::json sub = {{"type", "subscribe"}, {"channel", channel}};
            if (include_product_ids) {
                sub["product_ids"] = nlohmann::json::array({venue_symbols_.coinbase});
            }
            if (have_credentials) {
                const std::string jwt = generate_jwt(api_key, api_secret);
                if (!jwt.empty()) {
                    sub["jwt"] = jwt;
                } else {
                    LOG_ERROR("[Coinbase] JWT generation failed despite credentials being present — "
                              "sending unauthenticated subscribe (snapshot will likely time out)",
                              "symbol", symbol_.c_str(), "channel", channel);
                }
            }
            messages.push_back(sub.dump());
        };

        make_sub("heartbeats", false);
        make_sub("level2", true);
        make_sub("market_trades", true);
        return messages;
    }

    auto CoinbaseFeedHandler::fetch_tick_size() -> Result {
        const std::string url = api_url_ + "/api/v3/brokerage/market/products/" + venue_symbols_.coinbase;
        auto resp = http::get(url, {});
        if (!resp.ok() || resp.body.empty()) {
            LOG_WARN("[Coinbase] fetch_tick_size failed", "symbol", symbol_.c_str(), "status", resp.status);
            return Result::ERROR_CONNECTION_LOST;
        }
        auto json = nlohmann::json::parse(resp.body, nullptr, false);
        if (json.is_discarded()) {
            LOG_WARN("[Coinbase] fetch_tick_size JSON parse failed", "symbol", symbol_.c_str());
            return Result::ERROR_BOOK_CORRUPTED;
        }
        std::string ts = json.value("quote_increment", std::string(""));
        if (ts.empty()) {
            LOG_WARN("[Coinbase] fetch_tick_size: quote_increment missing", "symbol", symbol_.c_str());
            return Result::ERROR_BOOK_CORRUPTED;
        }
        tick_size_ = tick_from_string(ts);
        LOG_INFO("[Coinbase] Tick size fetched", "symbol", symbol_.c_str(), "tick_size", tick_size_);
        return Result::SUCCESS;
    }

    void CoinbaseFeedHandler::emit_ops_event() {
        const int64_t ts_ns = http::now_ns();
        char json_buf[256];
        std::snprintf(json_buf, sizeof(json_buf),
                      "{\"event\":\"coinbase_feed_failed\",\"timestamp_ns\":%lld,\"symbol\":\"%s\"}\n",
                      static_cast<long long>(ts_ns), symbol_.c_str());
        std::fprintf(stdout, "[OPS_EVENT] coinbase_feed_failed: {\"symbol\":\"%s\"}\n",
                     symbol_.c_str());
        std::fflush(stdout);
        if (FILE *f = std::fopen("logs/ops_events.jsonl", "a")) {
            std::fputs(json_buf, f);
            std::fclose(f);
        }
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

        if (tick_size_ <= 0.0) {
            fetch_tick_size();
        }

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
            if (auto *ctx =
                    static_cast<struct lws_context *>(lws_ctx_.load(std::memory_order_acquire))) {
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

        if (auto *ctx = static_cast<struct lws_context *>(lws_ctx_.load(std::memory_order_acquire))) {
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

        std::string ws_host = "advanced-trade-ws.coinbase.com";
        int ws_port = 443;
        std::string ws_path = "/";

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
            if (slash != std::string::npos) {
                ws_path = ws_url_.substr(slash);
            }

            size_t colon = authority.rfind(':');
            if (colon != std::string::npos) {
                ws_host = authority.substr(0, colon);
                ws_port = std::stoi(authority.substr(colon + 1));
            } else if (!authority.empty()) {
                ws_host = authority;
            }
        }

        while (running_.load(std::memory_order_acquire)) {
            CoinbaseWsSession session;
            session.handler = this;
            session.subscribe_msgs = build_subscription_messages();

            lws_context_creation_info ctx_info = {};
            ctx_info.port = CONTEXT_PORT_NO_LISTEN;
            ctx_info.protocols = k_coinbase_protocols;
            ctx_info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
            ctx_info.user = &session;

            auto *ctx = lws_create_context(&ctx_info);
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
            connect_info.address = ws_host.c_str();
            connect_info.port = ws_port;
            connect_info.path = ws_path.c_str();
            connect_info.host = ws_host.c_str();
            connect_info.origin = ws_host.c_str();
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

    auto CoinbaseFeedHandler::process_snapshot(const nlohmann::json &json, uint64_t seq) -> Result {
        Snapshot snap;
        snap.symbol = symbol_;
        snap.exchange = Exchange::COINBASE;
        snap.sequence = seq;
        snap.timestamp_local_ns = http::now_ns();
        snap.timestamp_exchange_ns = extract_exchange_timestamp_ns(json);

        auto events_it = json.find("events");
        if (events_it == json.end() || !events_it->is_array()) {
            return Result::ERROR_BOOK_CORRUPTED;
        }

        for (const auto &event: *events_it) {
            auto updates_it = event.find("updates");
            if (updates_it == event.end() || !updates_it->is_array()) {
                continue;
            }

            for (const auto &upd: *updates_it) {
                auto side_it = upd.find("side");
                auto px_it = upd.find("price_level");
                auto qty_it = upd.find("new_quantity");
                if (side_it == upd.end() || px_it == upd.end() || qty_it == upd.end()) {
                    continue;
                }

                try {
                    PriceLevel level;
                    level.price = std::stod(px_it->get<std::string>());
                    level.size = std::stod(qty_it->get<std::string>());

                    const std::string side = side_it->get<std::string>();
                    if (side == "bid") {
                        snap.bids.push_back(level);
                    } else if (side == "offer" || side == "ask") {
                        snap.asks.push_back(level);
                    }
                } catch (const std::exception &ex) {
                    LOG_WARN("[Coinbase] snapshot level parse failed", "symbol", symbol_.c_str(), "error",
                             ex.what());
                }
            }
        }

        if (snap.bids.empty() || snap.asks.empty()) {
            return Result::ERROR_BOOK_CORRUPTED;
        }

        last_sequence_.store(seq, std::memory_order_release);
        last_event_time_exchange_ns_.store(snap.timestamp_exchange_ns, std::memory_order_release);
        if (snapshot_callback_) {
            snapshot_callback_(snap);
        }

        return Result::SUCCESS;
    }

    auto CoinbaseFeedHandler::process_delta(const nlohmann::json &json, uint64_t seq) -> Result {
        last_sequence_.store(seq, std::memory_order_release);
        const int64_t timestamp_local_ns = http::now_ns();
        const int64_t timestamp_exchange_ns = extract_exchange_timestamp_ns(json);
        last_event_time_exchange_ns_.store(timestamp_exchange_ns, std::memory_order_release);

        auto events_it = json.find("events");
        if (events_it == json.end() || !events_it->is_array()) {
            return Result::SUCCESS;
        }

        for (const auto &event: *events_it) {
            auto updates_it = event.find("updates");
            if (updates_it == event.end() || !updates_it->is_array()) {
                continue;
            }

            for (const auto &upd: *updates_it) {
                auto side_it = upd.find("side");
                auto px_it = upd.find("price_level");
                auto qty_it = upd.find("new_quantity");
                if (side_it == upd.end() || px_it == upd.end() || qty_it == upd.end()) {
                    continue;
                }

                try {
                    Delta delta;
                    delta.side = (side_it->get<std::string>() == "bid") ? Side::BID : Side::ASK;
                    delta.price = std::stod(px_it->get<std::string>());
                    delta.size = std::stod(qty_it->get<std::string>());
                    delta.sequence = seq;
                    delta.timestamp_exchange_ns = timestamp_exchange_ns;
                    delta.timestamp_local_ns = timestamp_local_ns;
                    if (delta_callback_) {
                        delta_callback_(delta);
                    }
                } catch (const std::exception &ex) {
                    LOG_WARN("[Coinbase] delta level parse failed", "symbol", symbol_.c_str(), "sequence",
                             seq, "error", ex.what());
                }
            }
        }

        return Result::SUCCESS;
    }

    auto CoinbaseFeedHandler::extract_exchange_timestamp_ns(const nlohmann::json &json) const
        -> int64_t {
        auto ts_it = json.find("timestamp");
        if (ts_it != json.end() && ts_it->is_string()) {
            return parse_rfc3339_ns(ts_it->get<std::string>());
        }

        auto events_it = json.find("events");
        if (events_it != json.end() && events_it->is_array()) {
            for (const auto &event: *events_it) {
                auto event_ts_it = event.find("event_time");
                if (event_ts_it != event.end() && event_ts_it->is_string()) {
                    return parse_rfc3339_ns(event_ts_it->get<std::string>());
                }
                auto current_time_it = event.find("current_time");
                if (current_time_it != event.end() && current_time_it->is_string()) {
                    return parse_rfc3339_ns(current_time_it->get<std::string>());
                }
            }
        }

        return 0;
    }

    auto CoinbaseFeedHandler::process_message(const std::string &message) -> Result {
        auto json = nlohmann::json::parse(message, nullptr, false);
        if (json.is_discarded()) {
            return Result::SUCCESS;
        }

        auto type_it = json.find("type");
        if (type_it != json.end() && type_it->is_string() && type_it->get<std::string>() == "error") {
            std::string reason = json.value("message", std::string("unknown error"));
            std::string reason_lower = reason;
            std::transform(reason_lower.begin(), reason_lower.end(), reason_lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (reason_lower.find("auth") != std::string::npos ||
                reason_lower.find("jwt") != std::string::npos ||
                reason_lower.find("signature") != std::string::npos) {
                auth_rejected_.store(true, std::memory_order_release);
                last_start_failure_reason_ = "authentication rejected: " + reason;
            } else {
                subscription_rejected_.store(true, std::memory_order_release);
                last_start_failure_reason_ = "subscription rejected: " + reason;
            }
            running_.store(false, std::memory_order_release);
            ws_cv_.notify_all();
            if (error_callback_) {
                error_callback_(last_start_failure_reason_);
            }
            return Result::ERROR_CONNECTION_LOST;
        }

        auto channel_it = json.find("channel");
        if (channel_it == json.end() || !channel_it->is_string()) {
            return Result::SUCCESS;
        }

        const std::string channel = channel_it->get<std::string>();
        if (channel == "subscriptions") {
            return Result::SUCCESS;
        }
        if (channel == "heartbeats") {
            const int64_t exchange_ts = extract_exchange_timestamp_ns(json);
            last_heartbeat_ns_.store(http::now_ns(), std::memory_order_release);
            if (exchange_ts > 0) {
                last_event_time_exchange_ns_.store(exchange_ts, std::memory_order_release);
            }
            return Result::SUCCESS;
        }
        if (channel == "market_trades") {
            auto events_it = json.find("events");
            if (events_it == json.end() || !events_it->is_array()) {
                return Result::SUCCESS;
            }
            for (const auto &event: *events_it) {
                auto trades_it = event.find("trades");
                if (trades_it == event.end() || !trades_it->is_array()) {
                    continue;
                }
                for (const auto &trade_json: *trades_it) {
                    auto price_it = trade_json.find("price");
                    auto size_it = trade_json.find("size");
                    if (price_it == trade_json.end() || size_it == trade_json.end()) {
                        continue;
                    }
                    try {
                        TradeFlow trade;
                        trade.last_trade_price = std::stod(price_it->get<std::string>());
                        trade.last_trade_size = std::stod(size_it->get<std::string>());
                        const std::string side = trade_json.value("side", std::string());
                        if (side == "BUY") {
                            trade.trade_direction = 0;
                        } else if (side == "SELL") {
                            trade.trade_direction = 1;
                        }
                        if (trade_callback_ != nullptr) {
                            trade_callback_(trade);
                        }
                    } catch (const std::exception &) {
                    }
                }
            }
            return Result::SUCCESS;
        }
        if (channel != "l2_data") {
            return Result::SUCCESS;
        }

        uint64_t seq = 0;
        auto seq_it = json.find("sequence_num");
        if (seq_it != json.end()) {
            if (seq_it->is_string()) {
                seq = std::stoull(seq_it->get<std::string>());
            } else {
                seq = seq_it->get<uint64_t>();
            }
        }

        bool has_snapshot = false;
        auto events_it = json.find("events");
        if (events_it != json.end() && events_it->is_array()) {
            for (const auto &event: *events_it) {
                auto event_type_it = event.find("type");
                if (event_type_it != event.end() && event_type_it->is_string() &&
                    event_type_it->get<std::string>() == "snapshot") {
                    has_snapshot = true;
                    break;
                }
            }
        }

        if (has_snapshot) {
            Result result = process_snapshot(json, seq);
            if (result != Result::SUCCESS) {
                last_start_failure_reason_ = "invalid snapshot payload";
                trigger_resnapshot("Invalid Coinbase snapshot");
                return result;
            }

            if (apply_buffered_deltas() != Result::SUCCESS) {
                last_start_failure_reason_ = "buffered delta replay failed";
                trigger_resnapshot("Buffered Coinbase deltas invalid after snapshot");
                return Result::ERROR_SEQUENCE_GAP;
            }

            state_.store(State::STREAMING, std::memory_order_release);
            ws_cv_.notify_all();
            return Result::SUCCESS;
        }

        auto cur = state_.load(std::memory_order_acquire);
        if (cur == State::BUFFERING || cur == State::DISCONNECTED) {
            if (delta_buffer_.size() < MAX_BUFFER_SIZE) {
                delta_buffer_.push_back(message);
            } else {
                last_start_failure_reason_ = "buffer overflow before snapshot";
                trigger_resnapshot("Buffer overflow");
            }
            return Result::SUCCESS;
        }

        if (!validate_delta_sequence(seq)) {
            last_start_failure_reason_ = "sequence gap resync";
            trigger_resnapshot("Coinbase sequence gap");
            return Result::ERROR_SEQUENCE_GAP;
        }

        return process_delta(json, seq);
    }

    auto CoinbaseFeedHandler::apply_buffered_deltas() -> Result {
        size_t applied = 0;
        size_t skipped = 0;

        for (const auto &msg: delta_buffer_) {
            auto json = nlohmann::json::parse(msg, nullptr, false);
            if (json.is_discarded()) {
                continue;
            }

            uint64_t seq = 0;
            auto seq_it = json.find("sequence_num");
            if (seq_it != json.end()) {
                if (seq_it->is_string()) {
                    seq = std::stoull(seq_it->get<std::string>());
                } else {
                    seq = seq_it->get<uint64_t>();
                }
            }

            uint64_t last = last_sequence_.load(std::memory_order_acquire);
            if (seq <= last) {
                ++skipped;
                continue;
            }

            if (seq != last + 1) {
                delta_buffer_.clear();
                return Result::ERROR_SEQUENCE_GAP;
            }

            if (process_delta(json, seq) == Result::SUCCESS) {
                ++applied;
            }
        }

        delta_buffer_.clear();
        LOG_INFO("[Coinbase] Applied Coinbase buffered deltas", "applied", applied, "skipped", skipped);
        return Result::SUCCESS;
    }

    auto CoinbaseFeedHandler::validate_delta_sequence(uint64_t seq) const -> bool {
        uint64_t last = last_sequence_.load(std::memory_order_acquire);
        return seq == last + 1;
    }

    void CoinbaseFeedHandler::trigger_resnapshot(const std::string &reason) {
        LOG_ERROR("[Coinbase] Triggering re-sync", "reason", reason.c_str());
        if (error_callback_) {
            error_callback_("Re-sync: " + reason);
        }
        delta_buffer_.clear();
        state_.store(State::BUFFERING, std::memory_order_release);
        reconnect_requested_.store(true, std::memory_order_release);
        if (auto *ctx = static_cast<struct lws_context *>(lws_ctx_.load(std::memory_order_acquire))) {
            lws_cancel_service(ctx);
        }
    }
}
