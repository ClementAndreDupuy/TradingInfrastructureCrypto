#include "kraken_feed_handler.hpp"
#include "../../common/rest_client.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <libwebsockets.h>

namespace trading {

// ─── Per-connection session ──────────────────────────────────────────────────
struct KrakenWsSession {
    KrakenFeedHandler* handler;
    struct lws* wsi{nullptr};
    bool established{false};
    bool closed{false};
    bool send_ping{false};
    bool send_subscribe{false};
    int64_t last_ping_ns{0};
    std::string subscribe_msg;
    char frag_buf[131072];
    size_t frag_len{0};
};

// ─── libwebsockets callback ──────────────────────────────────────────────────
static auto kraken_lws_cb(struct lws* wsi, enum lws_callback_reasons reason, void* /*user*/,
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
        const char* data = static_cast<const char*>(input);
        bool is_final = lws_is_final_fragment(wsi) != 0;
        if (session->frag_len + len < sizeof(session->frag_buf)) {
            std::memcpy(session->frag_buf + session->frag_len, data, len);
            session->frag_len += len;
        }
        if (is_final && (session->handler != nullptr)) {
            session->handler->process_message(std::string(session->frag_buf, session->frag_len));
            session->frag_len = 0;
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

// ─── Constructor / destructor ────────────────────────────────────────────────
KrakenFeedHandler::KrakenFeedHandler(const std::string& symbol, const std::string& api_key,
                                     const std::string& api_secret, const std::string& api_url,
                                     const std::string& ws_url)
    : symbol_(symbol), api_key_(api_key.empty() ? get_api_key_from_env() : api_key),
      api_secret_(api_secret.empty() ? get_api_secret_from_env() : api_secret), api_url_(api_url),
      ws_url_(ws_url) {
    LOG_INFO("KrakenFeedHandler created (public data, no auth required)", "symbol",
             symbol_.c_str());
}

auto KrakenFeedHandler::get_api_key_from_env() -> std::string {
    return http::env_var("KRAKEN_API_KEY");
}
auto KrakenFeedHandler::get_api_secret_from_env() -> std::string {
    return http::env_var("KRAKEN_API_SECRET");
}

KrakenFeedHandler::~KrakenFeedHandler() { stop(); }

// ─── Public API ──────────────────────────────────────────────────────────────
auto KrakenFeedHandler::start() -> Result {
    if (running_.load(std::memory_order_acquire)) {
        return Result::SUCCESS;
    }

    LOG_INFO("Starting feed handler", "symbol", symbol_.c_str());
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

    LOG_INFO("Feed handler started", "symbol", symbol_.c_str());
    return Result::SUCCESS;
}

void KrakenFeedHandler::stop() {
    if (!running_.load(std::memory_order_acquire) && !ws_thread_.joinable()) {
        return;
    }

    LOG_INFO("Stopping feed handler", "symbol", symbol_.c_str());
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

// ─── WebSocket event loop ─────────────────────────────────────────────────────
void KrakenFeedHandler::ws_event_loop() {
    constexpr uint32_t MAX_DELAY_MS = 30000;
    uint32_t delay_ms = 100;

    // Parse host/port from ws_url_: wss://host:port/...
    std::string ws_host = "ws.kraken.com";
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

    // Kraken v2 subscription: symbol format "XBT/USD" (insert '/' before last 3 chars).
    std::string ws_sym = symbol_;
    if (ws_sym.find('/') == std::string::npos && ws_sym.size() > 3) {
        ws_sym.insert(ws_sym.size() - 3, "/");
    }

    const std::string subscribe_msg =
        R"({"method":"subscribe","params":{"channel":"book","symbol":[")" + ws_sym +
        R"("],"depth":500}})";

    while (running_.load(std::memory_order_acquire)) {
        KrakenWsSession session;
        session.handler = this;
        session.send_subscribe = true;
        session.subscribe_msg = subscribe_msg;

        lws_context_creation_info ctx_info = {};
        ctx_info.port = CONTEXT_PORT_NO_LISTEN;
        ctx_info.protocols = k_kraken_protocols;
        ctx_info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
        ctx_info.user = &session;

        auto* ctx = lws_create_context(&ctx_info);
        if (ctx == nullptr) {
            LOG_ERROR("lws_create_context failed", "symbol", symbol_.c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
            continue;
        }
        lws_ctx_.store(ctx, std::memory_order_release);

        lws_client_connect_info connect_info = {};
        connect_info.context = ctx;
        connect_info.address = ws_host.c_str();
        connect_info.port = ws_port;
        connect_info.path = "/v2";
        connect_info.host = ws_host.c_str();
        connect_info.origin = ws_host.c_str();
        connect_info.protocol = k_kraken_protocols[0].name;
        connect_info.ssl_connection = LCCSCF_USE_SSL;

        if (lws_client_connect_via_info(&connect_info) == nullptr) {
            LOG_ERROR("WebSocket connect failed", "symbol", symbol_.c_str());
            lws_context_destroy(ctx);
            lws_ctx_.store(nullptr, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
            continue;
        }

        // Wait for ESTABLISHED (subscription sent in WRITEABLE callback).
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

        LOG_INFO("WebSocket established, fetching Kraken snapshot", "symbol", symbol_.c_str());
        state_.store(State::BUFFERING, std::memory_order_release);
        delta_buffer_.clear();
        last_seq_.store(0, std::memory_order_release);

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
        delay_ms = 100;

        ws_cv_.notify_all();

        // ── Stream phase ──────────────────────────────────────────────────
        while (running_.load() && !session.closed) {
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

        lws_context_destroy(ctx);
        lws_ctx_.store(nullptr, std::memory_order_release);

        if (running_.load()) {
            LOG_WARN("Kraken WebSocket disconnected, reconnecting", "symbol", symbol_.c_str(),
                     "delay_ms", delay_ms);
            trigger_resnapshot("WebSocket disconnected");
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
        }
    }
}

// ─── REST snapshot ────────────────────────────────────────────────────────────
// Kraken REST response: {"error":[],"result":{"XXBTZUSD":{"bids":[["px","vol",ts],...],...}}}
// The result key is the internal pair name (may differ from the requested symbol).
auto KrakenFeedHandler::fetch_snapshot() -> Result {
    const std::string url = api_url_ + "/0/public/Depth?pair=" + symbol_ + "&count=500";

    LOG_INFO("Fetching Kraken snapshot", "symbol", symbol_.c_str());

    auto resp = http::get(url);
    if (resp.body.empty()) {
        LOG_ERROR("Snapshot fetch failed", "symbol", symbol_.c_str(), "status", resp.status);
        return Result::ERROR_CONNECTION_LOST;
    }

    auto json = nlohmann::json::parse(resp.body, nullptr, false);
    if (json.is_discarded()) {
        LOG_ERROR("Snapshot JSON parse failed", "symbol", symbol_.c_str());
        return Result::ERROR_BOOK_CORRUPTED;
    }

    // error must be an empty array
    auto err_it = json.find("error");
    if (err_it == json.end() || !err_it->is_array() || !err_it->empty()) {
        LOG_ERROR("Kraken API returned error", "symbol", symbol_.c_str());
        return Result::ERROR_BOOK_CORRUPTED;
    }

    auto res_it = json.find("result");
    if (res_it == json.end() || !res_it->is_object() || res_it->empty()) {
        LOG_ERROR("No result in snapshot", "symbol", symbol_.c_str());
        return Result::ERROR_BOOK_CORRUPTED;
    }

    // result has one key (internal pair name, e.g., XXBTZUSD)
    const auto pair_data = res_it->begin().value();

    const char* symbol = symbol_.c_str();
    auto parse_rest_levels = [symbol](const nlohmann::json& arr) -> std::vector<PriceLevel> {
        std::vector<PriceLevel> levels;
        if (!arr.is_array()) {
            return levels;
        }
        levels.reserve(arr.size());
        for (const auto& lvl : arr) {
            // Each entry: ["price_str", "vol_str", timestamp_int]
            if (!lvl.is_array() || lvl.size() < 2) {
                continue;
            }
            try {
                levels.push_back(
                    {std::stod(lvl[0].get<std::string>()), std::stod(lvl[1].get<std::string>())});
            } catch (const std::exception& ex) {
                LOG_WARN("Kraken snapshot level parse failed", "symbol", symbol, "error",
                         ex.what());
            }
        }
        return levels;
    };

    Snapshot snap;
    snap.symbol = symbol_;
    snap.exchange = Exchange::KRAKEN;
    snap.timestamp_local_ns = http::now_ns();
    snap.sequence = 0;
    snap.bids = parse_rest_levels(pair_data.value("bids", nlohmann::json::array()));
    snap.asks = parse_rest_levels(pair_data.value("asks", nlohmann::json::array()));

    if (snap.bids.empty() || snap.asks.empty()) {
        LOG_ERROR("Empty order book in snapshot", "symbol", symbol_.c_str());
        return Result::ERROR_BOOK_CORRUPTED;
    }

    last_seq_.store(0, std::memory_order_release);
    if (snapshot_callback_) {
        snapshot_callback_(snap);
    }

    LOG_INFO("Kraken snapshot received", "symbol", symbol_.c_str(), "bids", snap.bids.size(),
             "asks", snap.asks.size());
    return Result::SUCCESS;
}

// ─── Message dispatch ─────────────────────────────────────────────────────────
auto KrakenFeedHandler::process_message(const std::string& message) -> Result {
    auto json = nlohmann::json::parse(message, nullptr, false);
    if (json.is_discarded()) {
        return Result::SUCCESS;
    }

    auto ch_it = json.find("channel");
    if (ch_it == json.end() || !ch_it->is_string() || ch_it->get<std::string>() != "book") {
        return Result::SUCCESS;
    }

    auto seq_it = json.find("seq");
    if (seq_it == json.end()) {
        return Result::ERROR_INVALID_SEQUENCE;
    }
    uint64_t seq = seq_it->get<uint64_t>();

    auto cur = state_.load(std::memory_order_acquire);

    if (cur == State::BUFFERING) {
        if (delta_buffer_.size() < MAX_BUFFER_SIZE) {
            delta_buffer_.push_back(message);
        } else {
            trigger_resnapshot("Buffer overflow");
        }
        return Result::SUCCESS;
    }

    if (!validate_delta_sequence(seq)) {
        LOG_ERROR("Kraken sequence gap", "expected", last_seq_.load() + 1, "seq", seq);
        trigger_resnapshot("Sequence gap");
        return Result::ERROR_SEQUENCE_GAP;
    }

    return process_delta(json, seq);
}

// ─── Delta processing ─────────────────────────────────────────────────────────
// Kraken v2 book update: {"channel":"book","type":"update","seq":N,
//   "data":[{"symbol":"BTC/USD","bids":[{"price":X,"qty":Y}],"asks":[...]}]}
auto KrakenFeedHandler::process_delta(const nlohmann::json& json, uint64_t seq) -> Result {
    last_seq_.store(seq, std::memory_order_release);

    int64_t timestamp_ns = http::now_ns();

    // Levels may be in data[0] (v2 envelope) or at the top level.
    const nlohmann::json* src = &json;
    auto data_it = json.find("data");
    if (data_it != json.end() && data_it->is_array() && !data_it->empty()) {
        src = &(*data_it)[0];
    }

    auto emit_levels = [&](const nlohmann::json& arr, Side side) -> void {
        if (!arr.is_array()) {
            return;
        }
        for (const auto& lvl : arr) {
            auto p_it = lvl.find("price");
            auto q_it = lvl.find("qty");
            if (p_it == lvl.end() || q_it == lvl.end()) {
                continue;
            }
            Delta delta;
            delta.side = side;
            delta.price = p_it->get<double>();
            delta.size = q_it->get<double>();
            delta.sequence = seq;
            delta.timestamp_local_ns = timestamp_ns;
            if (delta_callback_) {
                delta_callback_(delta);
            }
        }
    };

    emit_levels(src->value("bids", nlohmann::json::array()), Side::BID);
    emit_levels(src->value("asks", nlohmann::json::array()), Side::ASK);

    return Result::SUCCESS;
}

// ─── Buffered-delta replay ────────────────────────────────────────────────────
auto KrakenFeedHandler::apply_buffered_deltas() -> Result {
    size_t applied = 0, skipped = 0;

    for (const auto& msg : delta_buffer_) {
        auto json = nlohmann::json::parse(msg, nullptr, false);
        if (json.is_discarded()) {
            continue;
        }

        auto seq_it = json.find("seq");
        if (seq_it == json.end()) {
            continue;
        }

        uint64_t seq = seq_it->get<uint64_t>();
        uint64_t last_seq = last_seq_.load(std::memory_order_acquire);

        if (seq <= last_seq) {
            ++skipped;
        } else if (seq == last_seq + 1) {
            if (process_delta(json, seq) == Result::SUCCESS) {
                ++applied;
            }
        } else {
            LOG_ERROR("Gap in buffered deltas", "seq", seq, "expected", last_seq + 1);
            delta_buffer_.clear();
            return Result::ERROR_SEQUENCE_GAP;
        }
    }

    delta_buffer_.clear();
    LOG_INFO("Applied buffered deltas", "applied", applied, "skipped", skipped);
    return Result::SUCCESS;
}

// ─── Helpers ──────────────────────────────────────────────────────────────────
auto KrakenFeedHandler::validate_delta_sequence(uint64_t seq) -> bool {
    return seq == last_seq_.load(std::memory_order_acquire) + 1;
}

void KrakenFeedHandler::trigger_resnapshot(const std::string& reason) {
    LOG_WARN("Triggering re-snapshot", "reason", reason.c_str());
    if (error_callback_) {
        error_callback_("Re-snapshot: " + reason);
    }
    delta_buffer_.clear();
    state_.store(State::BUFFERING, std::memory_order_release);
}

} // namespace trading
