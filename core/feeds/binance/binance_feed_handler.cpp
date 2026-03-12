#include "binance_feed_handler.hpp"
#include "../../common/rest_client.hpp"
#include <libwebsockets.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cctype>
#include <thread>

namespace trading {

// ─── Per-connection session (lives on ws_event_loop stack) ──────────────────
struct BinanceWsSession {
    BinanceFeedHandler* handler;
    struct lws*         wsi{nullptr};
    bool established{false};
    bool closed{false};
    bool send_ping{false};
    int64_t last_ping_ns{0};
    char    frag_buf[131072];
    size_t  frag_len{0};
};

// ─── libwebsockets callback ──────────────────────────────────────────────────
static int binance_lws_cb(struct lws* wsi,
                          enum lws_callback_reasons reason,
                          void* /*user*/, void* in, size_t len)
{
    auto* s = static_cast<BinanceWsSession*>(
        lws_context_user(lws_get_context(wsi)));
    if (!s) return 0;

    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        s->wsi           = wsi;
        s->established   = true;
        s->last_ping_ns  = http::now_ns();
        lws_cancel_service(lws_get_context(wsi));
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE: {
        const char* data    = static_cast<const char*>(in);
        bool        is_final = lws_is_final_fragment(wsi);
        if (s->frag_len + len < sizeof(s->frag_buf)) {
            std::memcpy(s->frag_buf + s->frag_len, data, len);
            s->frag_len += len;
        }
        if (is_final && s->handler) {
            s->handler->process_message(std::string(s->frag_buf, s->frag_len));
            s->frag_len = 0;
        }
        break;
    }

    case LWS_CALLBACK_CLIENT_WRITEABLE:
        if (s->send_ping) {
            unsigned char buf[LWS_PRE + 4] = {};
            lws_write(wsi, buf + LWS_PRE, 0, LWS_WRITE_PING);
            s->send_ping = false;
        }
        break;

    case LWS_CALLBACK_CLIENT_CLOSED:
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        s->closed = true;
        lws_cancel_service(lws_get_context(wsi));
        break;

    default:
        break;
    }
    return 0;
}

static struct lws_protocols k_binance_protocols[] = {
    {"binance-depth", binance_lws_cb, 0, 131072, 0, nullptr, 0},
    {nullptr, nullptr, 0, 0, 0, nullptr, 0}
};

// ─── Constructor / destructor ────────────────────────────────────────────────
BinanceFeedHandler::BinanceFeedHandler(
    const std::string& symbol,
    const std::string& api_key,
    const std::string& api_secret,
    const std::string& api_url,
    const std::string& ws_url)
    : symbol_(symbol),
      api_key_(api_key.empty() ? get_api_key_from_env() : api_key),
      api_secret_(api_secret.empty() ? get_api_secret_from_env() : api_secret),
      api_url_(api_url),
      ws_url_(ws_url)
{
    if (!api_key_.empty()) {
        LOG_INFO("BinanceFeedHandler created with API key", "symbol", symbol_.c_str());
    } else {
        LOG_INFO("BinanceFeedHandler created (public data only)", "symbol", symbol_.c_str());
    }
}

std::string BinanceFeedHandler::get_api_key_from_env()    { return http::env_var("BINANCE_API_KEY"); }
std::string BinanceFeedHandler::get_api_secret_from_env() { return http::env_var("BINANCE_API_SECRET"); }

BinanceFeedHandler::~BinanceFeedHandler() { stop(); }

// ─── Public API ──────────────────────────────────────────────────────────────
Result BinanceFeedHandler::start() {
    if (running_.load(std::memory_order_acquire)) return Result::SUCCESS;

    LOG_INFO("Starting feed handler", "symbol", symbol_.c_str());
    running_.store(true, std::memory_order_release);
    state_.store(State::BUFFERING, std::memory_order_release);

    ws_thread_ = std::thread([this]() { ws_event_loop(); });

    // Block until STREAMING state (first sync complete) or failure (30s timeout).
    std::unique_lock<std::mutex> lk(ws_mutex_);
    bool ready = ws_cv_.wait_for(lk, std::chrono::seconds(30), [this]() {
        return state_.load(std::memory_order_acquire) == State::STREAMING
            || !running_.load(std::memory_order_acquire);
    });

    if (!ready || !running_.load(std::memory_order_acquire)) {
        running_.store(false, std::memory_order_release);
        if (auto* ctx = static_cast<struct lws_context*>(
                lws_ctx_.load(std::memory_order_acquire))) {
            lws_cancel_service(ctx);
        }
        ws_cv_.notify_all();
        if (ws_thread_.joinable()) ws_thread_.join();
        return Result::ERROR_CONNECTION_LOST;
    }

    LOG_INFO("Feed handler started", "symbol", symbol_.c_str());
    return Result::SUCCESS;
}

void BinanceFeedHandler::stop() {
    if (!running_.load(std::memory_order_acquire) && !ws_thread_.joinable()) return;

    LOG_INFO("Stopping feed handler", "symbol", symbol_.c_str());
    running_.store(false, std::memory_order_release);
    state_.store(State::DISCONNECTED, std::memory_order_release);

    if (auto* ctx = static_cast<struct lws_context*>(
            lws_ctx_.load(std::memory_order_acquire))) {
        lws_cancel_service(ctx);
    }
    ws_cv_.notify_all();

    if (ws_thread_.joinable()) ws_thread_.join();
}

// ─── WebSocket event loop (runs in ws_thread_) ────────────────────────────────
void BinanceFeedHandler::ws_event_loop() {
    constexpr uint32_t MAX_DELAY_MS = 30000;
    uint32_t delay_ms = 100;

    // Build stream path  /ws/<symbol_lower>@depth@100ms
    std::string sym_lower = symbol_;
    for (auto& c : sym_lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string ws_path = "/ws/" + sym_lower + "@depth@100ms";

    // Parse host and port from ws_url_: wss://host:port/...
    std::string ws_host = "stream.binance.com";
    int         ws_port = 9443;
    {
        size_t pos = ws_url_.find("://");
        if (pos != std::string::npos) pos += 3; else pos = 0;
        size_t slash = ws_url_.find('/', pos);
        std::string authority = ws_url_.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            ws_host = authority.substr(0, colon);
            ws_port = std::stoi(authority.substr(colon + 1));
        } else if (!authority.empty()) {
            ws_host = authority;
        }
    }

    while (running_.load(std::memory_order_acquire)) {
        BinanceWsSession session;
        session.handler = this;

        lws_context_creation_info ctx_info = {};
        ctx_info.port      = CONTEXT_PORT_NO_LISTEN;
        ctx_info.protocols = k_binance_protocols;
        ctx_info.options   = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
        ctx_info.user      = &session;

        auto* ctx = lws_create_context(&ctx_info);
        if (!ctx) {
            LOG_ERROR("lws_create_context failed", "symbol", symbol_.c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
            continue;
        }
        lws_ctx_.store(ctx, std::memory_order_release);

        lws_client_connect_info cc = {};
        cc.context      = ctx;
        cc.address      = ws_host.c_str();
        cc.port         = ws_port;
        cc.path         = ws_path.c_str();
        cc.host         = ws_host.c_str();
        cc.origin       = ws_host.c_str();
        cc.protocol     = k_binance_protocols[0].name;
        cc.ssl_connection = LCCSCF_USE_SSL;

        if (!lws_client_connect_via_info(&cc)) {
            LOG_ERROR("WebSocket connect failed", "symbol", symbol_.c_str());
            lws_context_destroy(ctx);
            lws_ctx_.store(nullptr, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
            continue;
        }

        // Wait for ESTABLISHED or error.
        while (running_.load() && !session.established && !session.closed)
            lws_service(ctx, 50);

        if (!running_.load() || session.closed) {
            lws_context_destroy(ctx);
            lws_ctx_.store(nullptr, std::memory_order_release);
            if (running_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
            }
            continue;
        }

        LOG_INFO("WebSocket established, fetching snapshot", "symbol", symbol_.c_str());
        state_.store(State::BUFFERING, std::memory_order_release);
        delta_buffer_.clear();

        // Fetch REST snapshot (blocking; WebSocket buffers deltas meanwhile).
        if (fetch_snapshot() != Result::SUCCESS) {
            lws_context_destroy(ctx);
            lws_ctx_.store(nullptr, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
            continue;
        }

        state_.store(State::SYNCHRONIZED, std::memory_order_release);
        apply_buffered_deltas();
        state_.store(State::STREAMING, std::memory_order_release);
        delay_ms = 100;  // reset backoff on successful sync

        // Unblock start().
        ws_cv_.notify_all();

        // ── Stream phase ──────────────────────────────────────────────────
        while (running_.load() && !session.closed) {
            lws_service(ctx, 1000);

            if (!session.closed && session.wsi) {
                int64_t now = http::now_ns();
                if (now - session.last_ping_ns > 30'000'000'000LL) {
                    session.send_ping    = true;
                    session.last_ping_ns = now;
                    lws_callback_on_writable(session.wsi);
                }
            }
        }

        lws_context_destroy(ctx);
        lws_ctx_.store(nullptr, std::memory_order_release);

        if (running_.load()) {
            LOG_WARN("WebSocket disconnected, reconnecting",
                     "symbol", symbol_.c_str(), "delay_ms", delay_ms);
            trigger_resnapshot("WebSocket disconnected");
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
        }
    }
}

// ─── REST snapshot ────────────────────────────────────────────────────────────
// Rate limit: Binance allows 1200 weight/min. Snapshot at limit=1000 costs
// weight 10. Enforce a minimum 1s cooldown between calls so reconnect storms
// (e.g. rapid disconnect/reconnect) don't ban the IP.
Result BinanceFeedHandler::fetch_snapshot() {
    using clock = std::chrono::steady_clock;
    auto now = clock::now();
    if (last_snapshot_time_ != clock::time_point{}) {
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_snapshot_time_).count();
        if (elapsed_ms < 1000)
            std::this_thread::sleep_for(std::chrono::milliseconds(1000 - elapsed_ms));
    }
    last_snapshot_time_ = clock::now();

    const std::string url =
        api_url_ + "/api/v3/depth?symbol=" + symbol_ + "&limit=1000";

    LOG_INFO("Fetching snapshot", "symbol", symbol_.c_str());

    auto resp = http::get(url, {"X-MBX-APIKEY: " + api_key_});
    if (resp.status == 429 || resp.status == 418) {
        LOG_ERROR("Binance rate limit hit, backing off",
                  "symbol", symbol_.c_str(), "status", resp.status);
        std::this_thread::sleep_for(std::chrono::seconds(60));
        return Result::ERROR_CONNECTION_LOST;
    }
    if (!resp.ok() || resp.body.empty()) {
        LOG_ERROR("Snapshot fetch failed", "symbol", symbol_.c_str(),
                  "status", resp.status);
        return Result::ERROR_CONNECTION_LOST;
    }

    auto j = nlohmann::json::parse(resp.body, nullptr, false);
    if (j.is_discarded()) {
        LOG_ERROR("Snapshot JSON parse failed", "symbol", symbol_.c_str());
        return Result::ERROR_BOOK_CORRUPTED;
    }

    auto lid_it = j.find("lastUpdateId");
    if (lid_it == j.end() || !lid_it->is_number_unsigned()) {
        LOG_ERROR("Bad lastUpdateId in snapshot", "symbol", symbol_.c_str());
        return Result::ERROR_BOOK_CORRUPTED;
    }

    Snapshot snap;
    snap.symbol             = symbol_;
    snap.exchange           = Exchange::BINANCE;
    snap.timestamp_local_ns = http::now_ns();
    snap.sequence           = lid_it->get<uint64_t>();

    auto parse_levels = [](const nlohmann::json& arr) {
        std::vector<PriceLevel> levels;
        if (!arr.is_array()) return levels;
        levels.reserve(arr.size());
        for (const auto& lvl : arr) {
            if (!lvl.is_array() || lvl.size() < 2) continue;
            try {
                levels.push_back({std::stod(lvl[0].get<std::string>()),
                                  std::stod(lvl[1].get<std::string>())});
            } catch (...) {}
        }
        return levels;
    };

    snap.bids = parse_levels(j.value("bids", nlohmann::json::array()));
    snap.asks = parse_levels(j.value("asks", nlohmann::json::array()));

    if (snap.bids.empty() || snap.asks.empty()) {
        LOG_ERROR("Empty order book in snapshot", "symbol", symbol_.c_str());
        return Result::ERROR_BOOK_CORRUPTED;
    }

    last_update_id_.store(snap.sequence, std::memory_order_release);
    if (snapshot_callback_) snapshot_callback_(snap);

    LOG_INFO("Snapshot received", "symbol", symbol_.c_str(),
             "sequence", snap.sequence,
             "bids", snap.bids.size(), "asks", snap.asks.size());
    return Result::SUCCESS;
}

// ─── Message dispatch ─────────────────────────────────────────────────────────
Result BinanceFeedHandler::process_message(const std::string& message) {
    auto j = nlohmann::json::parse(message, nullptr, false);
    if (j.is_discarded()) return Result::SUCCESS;

    auto e_it = j.find("e");
    if (e_it == j.end() || !e_it->is_string() ||
        e_it->get<std::string>() != "depthUpdate")
        return Result::SUCCESS;

    auto U_it = j.find("U");
    auto u_it = j.find("u");
    if (U_it == j.end() || u_it == j.end())
        return Result::ERROR_INVALID_SEQUENCE;

    uint64_t first_update_id = U_it->get<uint64_t>();
    uint64_t last_update_id  = u_it->get<uint64_t>();

    auto cur = state_.load(std::memory_order_acquire);

    if (cur == State::BUFFERING) {
        if (delta_buffer_.size() < MAX_BUFFER_SIZE) {
            delta_buffer_.push_back(message);
        } else {
            trigger_resnapshot("Buffer overflow");
        }
        return Result::SUCCESS;
    }

    if (!validate_delta_sequence(first_update_id, last_update_id)) {
        LOG_ERROR("Sequence gap",
                  "expected", last_update_id_.load() + 1, "U", first_update_id);
        trigger_resnapshot("Sequence gap");
        return Result::ERROR_SEQUENCE_GAP;
    }

    return process_delta(j, last_update_id);
}

// ─── Delta processing ─────────────────────────────────────────────────────────
Result BinanceFeedHandler::process_delta(const nlohmann::json& j, uint64_t seq) {
    last_update_id_.store(seq, std::memory_order_release);

    int64_t ts = http::now_ns();

    auto emit_levels = [&](const nlohmann::json& arr, Side side) {
        if (!arr.is_array()) return;
        for (const auto& lvl : arr) {
            if (!lvl.is_array() || lvl.size() < 2) continue;
            try {
                Delta d;
                d.side               = side;
                d.price              = std::stod(lvl[0].get<std::string>());
                d.size               = std::stod(lvl[1].get<std::string>());
                d.sequence           = seq;
                d.timestamp_local_ns = ts;
                if (delta_callback_) delta_callback_(d);
            } catch (...) {}
        }
    };

    // Binance depthUpdate: bids under "b", asks under "a".
    emit_levels(j.value("b", nlohmann::json::array()), Side::BID);
    emit_levels(j.value("a", nlohmann::json::array()), Side::ASK);

    LOG_DEBUG("Delta processed", "sequence", seq);
    return Result::SUCCESS;
}

// ─── Buffered-delta replay ────────────────────────────────────────────────────
Result BinanceFeedHandler::apply_buffered_deltas() {
    size_t applied = 0, skipped = 0;
    uint64_t last_id = last_update_id_.load(std::memory_order_acquire);

    for (const auto& msg : delta_buffer_) {
        auto j = nlohmann::json::parse(msg, nullptr, false);
        if (j.is_discarded()) continue;

        auto U_it = j.find("U");
        auto u_it = j.find("u");
        if (U_it == j.end() || u_it == j.end()) continue;

        uint64_t U = U_it->get<uint64_t>();
        uint64_t u = u_it->get<uint64_t>();

        if (U <= last_id + 1 && last_id + 1 <= u) {
            if (process_delta(j, u) == Result::SUCCESS) ++applied;
        } else if (u <= last_id) {
            ++skipped;
        } else {
            LOG_ERROR("Gap in buffered deltas", "U", U, "u", u);
            delta_buffer_.clear();
            return Result::ERROR_SEQUENCE_GAP;
        }
    }

    delta_buffer_.clear();
    LOG_INFO("Applied buffered deltas", "applied", applied, "skipped", skipped);
    return Result::SUCCESS;
}

// ─── Helpers ──────────────────────────────────────────────────────────────────
bool BinanceFeedHandler::validate_delta_sequence(uint64_t first_update_id,
                                                  uint64_t last_update_id) {
    uint64_t expected = last_update_id_.load(std::memory_order_acquire) + 1;
    return first_update_id <= expected && expected <= last_update_id;
}

void BinanceFeedHandler::trigger_resnapshot(const std::string& reason) {
    LOG_WARN("Triggering re-snapshot", "reason", reason.c_str());
    if (error_callback_) error_callback_("Re-snapshot: " + reason);
    delta_buffer_.clear();
    state_.store(State::BUFFERING, std::memory_order_release);
}

}  // namespace trading
