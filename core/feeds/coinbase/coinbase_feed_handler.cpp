#include "coinbase_feed_handler.hpp"
#include "../../common/rest_client.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <libwebsockets.h>

namespace trading {

struct CoinbaseWsSession {
    CoinbaseFeedHandler* handler;
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

static auto coinbase_lws_cb(struct lws* wsi, enum lws_callback_reasons reason, void* /*user*/,
                            void* input, size_t len) -> int {
    auto* session = static_cast<CoinbaseWsSession*>(lws_context_user(lws_get_context(wsi)));
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
        const char* data = static_cast<const char*>(input);
        bool is_final = lws_is_final_fragment(wsi) != 0;
        if (session->frag_len + len <= sizeof(session->frag_buf)) {
            std::memcpy(session->frag_buf + session->frag_len, data, len);
            session->frag_len += len;
        } else {
            // Message exceeds buffer: discard the entire fragmented message.
            session->frag_len = 0;
        }
        if (is_final && session->frag_len > 0 && (session->handler != nullptr)) {
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

static struct lws_protocols k_coinbase_protocols[] = {
    {"coinbase-l2", coinbase_lws_cb, 0, 131072, 0, nullptr, 0},
    {nullptr, nullptr, 0, 0, 0, nullptr, 0},
};

CoinbaseFeedHandler::CoinbaseFeedHandler(const std::string& symbol, const std::string& ws_url,
                                         const std::string& api_url)
    : symbol_(symbol), ws_url_(ws_url), api_url_(api_url),
      venue_symbols_(SymbolMapper::map_all(symbol)) {
    LOG_INFO("CoinbaseFeedHandler created", "symbol", symbol_.c_str());
}

CoinbaseFeedHandler::~CoinbaseFeedHandler() { stop(); }

// ─── Symbol info ─────────────────────────────────────────────────────────────
auto CoinbaseFeedHandler::fetch_tick_size() -> Result {
    const std::string url = api_url_ + "/products/" + venue_symbols_.coinbase;
    auto resp = http::get(url);
    if (!resp.ok() || resp.body.empty()) {
        LOG_WARN("fetch_tick_size failed", "symbol", symbol_.c_str(), "status", resp.status);
        return Result::ERROR_CONNECTION_LOST;
    }
    auto json = nlohmann::json::parse(resp.body, nullptr, false);
    if (json.is_discarded()) {
        LOG_WARN("fetch_tick_size JSON parse failed", "symbol", symbol_.c_str());
        return Result::ERROR_BOOK_CORRUPTED;
    }
    try {
        tick_size_ = std::stod(json.value("quote_increment", "0"));
        LOG_INFO("Tick size fetched", "symbol", symbol_.c_str(), "tick_size", tick_size_);
        return Result::SUCCESS;
    } catch (...) {
        LOG_WARN("fetch_tick_size: quote_increment parse failed", "symbol", symbol_.c_str());
        return Result::ERROR_BOOK_CORRUPTED;
    }
}

auto CoinbaseFeedHandler::start() -> Result {
    if (running_.load(std::memory_order_acquire)) {
        return Result::SUCCESS;
    }

    LOG_INFO("Starting Coinbase feed handler", "symbol", symbol_.c_str());
    fetch_tick_size();
    running_.store(true, std::memory_order_release);
    state_.store(State::BUFFERING, std::memory_order_release);
    last_sequence_.store(0, std::memory_order_release);
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

    LOG_INFO("Coinbase feed handler started", "symbol", symbol_.c_str());
    return Result::SUCCESS;
}

void CoinbaseFeedHandler::stop() {
    if (!running_.load(std::memory_order_acquire) && !ws_thread_.joinable()) {
        return;
    }

    LOG_INFO("Stopping Coinbase feed handler", "symbol", symbol_.c_str());
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

        nlohmann::json sub = {
            {"type", "subscribe"},
            {"product_ids", nlohmann::json::array({venue_symbols_.coinbase})},
            {"channel", "l2_data"},
        };
        session.subscribe_msg = sub.dump();

        lws_context_creation_info ctx_info = {};
        ctx_info.port = CONTEXT_PORT_NO_LISTEN;
        ctx_info.protocols = k_coinbase_protocols;
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
        connect_info.path = ws_path.c_str();
        connect_info.host = ws_host.c_str();
        connect_info.origin = ws_host.c_str();
        connect_info.protocol = k_coinbase_protocols[0].name;
        connect_info.ssl_connection = LCCSCF_USE_SSL;

        if (lws_client_connect_via_info(&connect_info) == nullptr) {
            LOG_ERROR("Coinbase WebSocket connect failed", "symbol", symbol_.c_str());
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

        LOG_INFO("Coinbase WebSocket established", "symbol", symbol_.c_str());
        delay_ms = 100;

        while (running_.load() && !session.closed) {
            if (reconnect_requested_.exchange(false, std::memory_order_acq_rel)) {
                session.closed = true;
                break;
            }
            int64_t now_ns = http::now_ns();
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
            LOG_WARN("Coinbase stream disconnected, reconnecting", "symbol", symbol_.c_str(),
                     "backoff_ms", delay_ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
        }
    }

    ws_cv_.notify_all();
}

auto CoinbaseFeedHandler::process_snapshot(const nlohmann::json& json, uint64_t seq) -> Result {
    Snapshot snap;
    snap.symbol = symbol_;
    snap.exchange = Exchange::COINBASE;
    snap.sequence = seq;
    snap.timestamp_local_ns = http::now_ns();

    auto events_it = json.find("events");
    if (events_it == json.end() || !events_it->is_array()) {
        return Result::ERROR_BOOK_CORRUPTED;
    }

    for (const auto& event : *events_it) {
        auto updates_it = event.find("updates");
        if (updates_it == event.end() || !updates_it->is_array()) {
            continue;
        }

        for (const auto& upd : *updates_it) {
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
            } catch (const std::exception& ex) {
                LOG_WARN("Coinbase snapshot level parse failed", "symbol", symbol_.c_str(), "error",
                         ex.what());
            }
        }
    }

    if (snap.bids.empty() || snap.asks.empty()) {
        return Result::ERROR_BOOK_CORRUPTED;
    }

    last_sequence_.store(seq, std::memory_order_release);
    if (snapshot_callback_) {
        snapshot_callback_(snap);
    }

    return Result::SUCCESS;
}

auto CoinbaseFeedHandler::process_update(const nlohmann::json& json, uint64_t seq) -> Result {
    last_sequence_.store(seq, std::memory_order_release);
    int64_t timestamp_ns = http::now_ns();

    auto events_it = json.find("events");
    if (events_it == json.end() || !events_it->is_array()) {
        return Result::SUCCESS;
    }

    for (const auto& event : *events_it) {
        auto updates_it = event.find("updates");
        if (updates_it == event.end() || !updates_it->is_array()) {
            continue;
        }

        for (const auto& upd : *updates_it) {
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
                delta.timestamp_local_ns = timestamp_ns;
                if (delta_callback_) {
                    delta_callback_(delta);
                }
            } catch (const std::exception& ex) {
                LOG_WARN("Coinbase delta level parse failed", "symbol", symbol_.c_str(), "sequence",
                         seq, "error", ex.what());
            }
        }
    }

    return Result::SUCCESS;
}

auto CoinbaseFeedHandler::process_message(const std::string& message) -> Result {
    auto json = nlohmann::json::parse(message, nullptr, false);
    if (json.is_discarded()) {
        return Result::SUCCESS;
    }

    auto channel_it = json.find("channel");
    auto type_it = json.find("type");
    if (channel_it == json.end() || type_it == json.end()) {
        return Result::SUCCESS;
    }

    const std::string channel = channel_it->get<std::string>();
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
        for (const auto& event : *events_it) {
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
            trigger_resnapshot("Invalid Coinbase snapshot");
            return result;
        }

        if (apply_buffered_deltas() != Result::SUCCESS) {
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
            trigger_resnapshot("Buffer overflow");
        }
        return Result::SUCCESS;
    }

    if (!validate_delta_sequence(seq)) {
        trigger_resnapshot("Coinbase sequence gap");
        return Result::ERROR_SEQUENCE_GAP;
    }

    return process_update(json, seq);
}

auto CoinbaseFeedHandler::apply_buffered_deltas() -> Result {
    size_t applied = 0;
    size_t skipped = 0;

    for (const auto& msg : delta_buffer_) {
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

        if (process_update(json, seq) == Result::SUCCESS) {
            ++applied;
        }
    }

    delta_buffer_.clear();
    LOG_INFO("Applied Coinbase buffered deltas", "applied", applied, "skipped", skipped);
    return Result::SUCCESS;
}

auto CoinbaseFeedHandler::validate_delta_sequence(uint64_t seq) const -> bool {
    uint64_t last = last_sequence_.load(std::memory_order_acquire);
    return seq == last + 1;
}

void CoinbaseFeedHandler::trigger_resnapshot(const std::string& reason) {
    LOG_WARN("Triggering Coinbase re-sync", "reason", reason.c_str());
    if (error_callback_) {
        error_callback_("Re-sync: " + reason);
    }
    delta_buffer_.clear();
    state_.store(State::BUFFERING, std::memory_order_release);
    reconnect_requested_.store(true, std::memory_order_release);
    if (auto* ctx = static_cast<struct lws_context*>(lws_ctx_.load(std::memory_order_acquire))) {
        lws_cancel_service(ctx);
    }
}

} // namespace trading
