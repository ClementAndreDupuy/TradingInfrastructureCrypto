#include "binance_feed_handler.hpp"
#include "../common/tick_size.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <exception>
#include <libwebsockets.h>
#include <thread>

namespace trading {

namespace {

auto fnv1a_bytes(const char* data, size_t len, uint32_t seed) noexcept -> uint32_t {
    uint32_t hash = seed;
    for (size_t i = 0; i < len; ++i) {
        hash ^= static_cast<uint8_t>(data[i]);
        hash *= 16777619u;
    }
    return hash;
}

auto compute_snapshot_checksum(const std::vector<PriceLevel>& bids,
                               const std::vector<PriceLevel>& asks) noexcept -> uint32_t {
    uint32_t hash = 2166136261u;
    auto hash_level = [&hash](const PriceLevel& level) -> void {
        hash = fnv1a_bytes(reinterpret_cast<const char*>(&level.price), sizeof(level.price), hash);
        hash = fnv1a_bytes(reinterpret_cast<const char*>(&level.size), sizeof(level.size), hash);
    };

    for (const auto& level : bids) {
        hash_level(level);
    }
    for (const auto& level : asks) {
        hash_level(level);
    }
    return hash;
}

auto parse_exchange_timestamp_ns(const nlohmann::json& json) -> int64_t {
    auto event_time_it = json.find("E");
    if (event_time_it == json.end() || !event_time_it->is_number_unsigned()) {
        return 0;
    }
    constexpr int64_t k_ns_per_ms = 1'000'000;
    return static_cast<int64_t>(event_time_it->get<uint64_t>()) * k_ns_per_ms;
}

}

struct BinanceWsSession {
    BinanceFeedHandler* handler;
    struct lws* wsi{nullptr};
    bool established{false};
    bool closed{false};
    bool send_ping{false};
    int64_t last_ping_ns{0};
    char frag_buf[131072];
    size_t frag_len{0};
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
        const char* data = static_cast<const char*>(input);
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

    std::string ws_host = "stream.binance.com";
    int ws_port = 9443;
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

    while (running_.load(std::memory_order_acquire)) {
        reconnect_requested_.store(false, std::memory_order_release);
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
        connect_info.protocol = k_binance_protocols[0].name;
        connect_info.ssl_connection = LCCSCF_USE_SSL;

        if (lws_client_connect_via_info(&connect_info) == nullptr) {
            LOG_ERROR("[Binance] WebSocket connect failed", "symbol", symbol_.c_str());
            lws_context_destroy(ctx);
            lws_ctx_.store(nullptr, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
            continue;
        }

        while (running_.load(std::memory_order_acquire) && !session.established && !session.closed) {
            lws_service(ctx, 50);
        }

        if (!running_.load(std::memory_order_acquire) || session.closed) {
            lws_context_destroy(ctx);
            lws_ctx_.store(nullptr, std::memory_order_release);
            if (running_.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
            }
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
            lws_context_destroy(ctx);
            lws_ctx_.store(nullptr, std::memory_order_release);
            continue;
        }

        if (!snapshot_ready) {
            lws_context_destroy(ctx);
            lws_ctx_.store(nullptr, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
            continue;
        }

        if (apply_buffered_deltas(last_sequence_.load(std::memory_order_acquire)) != Result::SUCCESS) {
            lws_context_destroy(ctx);
            lws_ctx_.store(nullptr, std::memory_order_release);
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

        lws_context_destroy(ctx);
        lws_ctx_.store(nullptr, std::memory_order_release);

        if (running_.load(std::memory_order_acquire)) {
            LOG_WARN("[Binance] WebSocket disconnected, reconnecting", "symbol", symbol_.c_str(), "delay_ms",
                     delay_ms);
            if (!reconnect_requested_.load(std::memory_order_acquire)) {
                trigger_resnapshot("WebSocket disconnected");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, MAX_DELAY_MS);
        }
    }
}

auto BinanceFeedHandler::process_snapshot() -> Result {
    using clock = std::chrono::steady_clock;
    auto now = clock::now();
    if (last_snapshot_time_ != clock::time_point{}) {
        auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_snapshot_time_).count();
        if (elapsed_ms < 1000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000 - elapsed_ms));
        }
    }
    last_snapshot_time_ = clock::now();

    const std::string url = api_url_ + "/api/v3/depth?symbol=" + venue_symbols_.binance + "&limit=5000";
    LOG_INFO("[Binance] Fetching snapshot", "symbol", symbol_.c_str());

    auto request_started = clock::now();
    auto resp = http::get(url);
    snapshot_latency_ms_ = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - request_started).count());
    if (resp.status == 429 || resp.status == 418) {
        LOG_ERROR("[Binance] Binance rate limit hit, backing off", "symbol", symbol_.c_str(), "status",
                  resp.status);
        std::this_thread::sleep_for(std::chrono::seconds(60));
        return Result::ERROR_CONNECTION_LOST;
    }
    if (!resp.ok() || resp.body.empty()) {
        LOG_ERROR("[Binance] Snapshot fetch failed", "symbol", symbol_.c_str(), "status", resp.status);
        return Result::ERROR_CONNECTION_LOST;
    }

    auto json = nlohmann::json::parse(resp.body, nullptr, false);
    if (json.is_discarded()) {
        LOG_ERROR("[Binance] Snapshot JSON parse failed", "symbol", symbol_.c_str());
        return Result::ERROR_BOOK_CORRUPTED;
    }

    auto lid_it = json.find("lastUpdateId");
    if (lid_it == json.end() || !lid_it->is_number_unsigned()) {
        LOG_ERROR("[Binance] Bad lastUpdateId in snapshot", "symbol", symbol_.c_str());
        return Result::ERROR_BOOK_CORRUPTED;
    }

    Snapshot snap;
    snap.symbol = symbol_;
    snap.exchange = Exchange::BINANCE;
    snap.timestamp_local_ns = http::now_ns();
    snap.sequence = lid_it->get<uint64_t>();

    const char* symbol = symbol_.c_str();
    auto parse_levels = [symbol](const nlohmann::json& arr) -> std::vector<PriceLevel> {
        std::vector<PriceLevel> levels;
        if (!arr.is_array()) {
            return levels;
        }
        levels.reserve(arr.size());
        for (const auto& lvl : arr) {
            if (!lvl.is_array() || lvl.size() < 2) {
                continue;
            }
            try {
                levels.push_back(
                    {std::stod(lvl[0].get<std::string>()), std::stod(lvl[1].get<std::string>())});
            } catch (const std::exception& ex) {
                LOG_WARN("[Binance] Binance snapshot level parse failed", "symbol", symbol, "error",
                         ex.what());
            }
        }
        return levels;
    };

    snap.bids = parse_levels(json.value("bids", nlohmann::json::array()));
    snap.asks = parse_levels(json.value("asks", nlohmann::json::array()));

    auto checksum_it = json.find("checksum");
    if (checksum_it != json.end() && checksum_it->is_number_unsigned()) {
        snap.checksum = checksum_it->get<uint32_t>();
        snap.checksum_present = true;

        const uint32_t local_checksum = compute_snapshot_checksum(snap.bids, snap.asks);
        if (local_checksum != snap.checksum) {
            LOG_ERROR("[Binance] Binance snapshot checksum mismatch", "symbol", symbol_.c_str(), "local",
                      local_checksum, "remote", snap.checksum);
            return Result::ERROR_BOOK_CORRUPTED;
        }
    }

    if (snap.bids.empty() || snap.asks.empty()) {
        LOG_ERROR("[Binance] Empty order book in snapshot", "symbol", symbol_.c_str());
        return Result::ERROR_BOOK_CORRUPTED;
    }

    last_sequence_.store(snap.sequence, std::memory_order_release);
    if (snapshot_callback_) {
        snapshot_callback_(snap);
    }

    LOG_INFO("[Binance] Snapshot received", "symbol", symbol_.c_str(), "sequence", snap.sequence, "bids",
             snap.bids.size(), "asks", snap.asks.size(), "snapshot_latency_ms", snapshot_latency_ms_);
    return Result::SUCCESS;
}

auto BinanceFeedHandler::parse_delta_message(const nlohmann::json& json, BufferedDelta& delta) const
    -> Result {
    auto first_update_it = json.find("U");
    auto last_update_it = json.find("u");
    if (first_update_it == json.end() || last_update_it == json.end() ||
        !first_update_it->is_number_unsigned() || !last_update_it->is_number_unsigned()) {
        return Result::ERROR_INVALID_SEQUENCE;
    }

    delta = {};
    delta.first_update_id = first_update_it->get<uint64_t>();
    delta.last_update_id = last_update_it->get<uint64_t>();
    delta.timestamp_exchange_ns = parse_exchange_timestamp_ns(json);

    auto parse_side = [](const nlohmann::json& levels_json, std::vector<ParsedLevel>& levels) -> Result {
        if (!levels_json.is_array()) {
            return Result::SUCCESS;
        }
        levels.reserve(levels_json.size());
        for (const auto& level_json : levels_json) {
            if (!level_json.is_array() || level_json.size() < 2) {
                continue;
            }
            try {
                levels.push_back(ParsedLevel{std::stod(level_json[0].get<std::string>()),
                                             std::stod(level_json[1].get<std::string>())});
            } catch (const std::exception&) {
                return Result::ERROR_BOOK_CORRUPTED;
            }
        }
        return Result::SUCCESS;
    };

    if (parse_side(json.value("b", nlohmann::json::array()), delta.bids) != Result::SUCCESS ||
        parse_side(json.value("a", nlohmann::json::array()), delta.asks) != Result::SUCCESS) {
        return Result::ERROR_BOOK_CORRUPTED;
    }

    return Result::SUCCESS;
}

auto BinanceFeedHandler::process_message(const std::string& message) -> Result {
    auto json = nlohmann::json::parse(message, nullptr, false);
    if (json.is_discarded()) {
        return Result::SUCCESS;
    }

    auto event_it = json.find("e");
    if (event_it == json.end() || !event_it->is_string() || event_it->get<std::string>() != "depthUpdate") {
        return Result::SUCCESS;
    }

    BufferedDelta delta;
    Result parse_result = parse_delta_message(json, delta);
    if (parse_result != Result::SUCCESS) {
        return parse_result;
    }

    State cur = state_.load(std::memory_order_acquire);
    if (cur == State::BUFFERING) {
        if (delta_buffer_.size() >= MAX_BUFFER_SIZE) {
            trigger_resnapshot("buffer_overflow");
            return Result::ERROR_CONNECTION_LOST;
        }
        delta_buffer_.push_back(std::move(delta));
        buffer_high_water_mark_ = std::max(buffer_high_water_mark_, delta_buffer_.size());
        return Result::SUCCESS;
    }

    const uint64_t current_sequence = last_sequence_.load(std::memory_order_acquire);
    if (delta.last_update_id <= current_sequence) {
        return Result::SUCCESS;
    }

    if (!validate_delta_sequence(delta.first_update_id, delta.last_update_id)) {
        LOG_ERROR("[Binance] Sequence gap", "expected", current_sequence + 1, "U", delta.first_update_id,
                  "u", delta.last_update_id);
        trigger_resnapshot("sequence_gap");
        return Result::ERROR_SEQUENCE_GAP;
    }

    return apply_delta(delta);
}

auto BinanceFeedHandler::apply_delta(const BufferedDelta& delta) -> Result {
    int64_t local_timestamp_ns = http::now_ns();

    auto emit_levels = [&](const std::vector<ParsedLevel>& levels, Side side) -> void {
        for (const auto& level : levels) {
            Delta book_delta;
            book_delta.side = side;
            book_delta.price = level.price;
            book_delta.size = level.size;
            book_delta.sequence = delta.last_update_id;
            book_delta.timestamp_exchange_ns = delta.timestamp_exchange_ns;
            book_delta.timestamp_local_ns = local_timestamp_ns;
            if (delta_callback_) {
                delta_callback_(book_delta);
            }
        }
    };

    emit_levels(delta.bids, Side::BID);
    emit_levels(delta.asks, Side::ASK);
    last_sequence_.store(delta.last_update_id, std::memory_order_release);

    LOG_DEBUG("Delta processed", "sequence", delta.last_update_id, "bids", delta.bids.size(), "asks",
              delta.asks.size());
    return Result::SUCCESS;
}

auto BinanceFeedHandler::apply_buffered_deltas(uint64_t snapshot_sequence) -> Result {
    size_t start_index = delta_buffer_.size();
    size_t skipped = 0;

    for (size_t i = 0; i < delta_buffer_.size(); ++i) {
        const auto& delta = delta_buffer_[i];
        if (delta.last_update_id <= snapshot_sequence) {
            ++skipped;
            continue;
        }
        if (delta.first_update_id <= snapshot_sequence + 1 && snapshot_sequence + 1 <= delta.last_update_id) {
            start_index = i;
            break;
        }
        LOG_WARN("[Binance] Buffered delta precedes sync point", "snapshot_sequence", snapshot_sequence,
                 "U", delta.first_update_id, "u", delta.last_update_id);
        trigger_resnapshot("snapshot_handoff_gap");
        return Result::ERROR_SEQUENCE_GAP;
    }

    if (start_index == delta_buffer_.size()) {
        if (delta_buffer_.empty() || skipped == delta_buffer_.size()) {
            buffered_skipped_ += static_cast<uint64_t>(skipped);
            LOG_INFO("[Binance] No buffered bridge delta required", "sequence", snapshot_sequence,
                     "skipped", skipped);
            delta_buffer_.clear();
            return Result::SUCCESS;
        }
        trigger_resnapshot("snapshot_handoff_missing_bridge");
        return Result::ERROR_SEQUENCE_GAP;
    }

    for (size_t i = start_index; i < delta_buffer_.size(); ++i) {
        const auto& delta = delta_buffer_[i];
        if (!validate_delta_sequence(delta.first_update_id, delta.last_update_id)) {
            LOG_ERROR("[Binance] Gap in buffered deltas", "expected",
                      last_sequence_.load(std::memory_order_acquire) + 1, "U", delta.first_update_id, "u",
                      delta.last_update_id);
            trigger_resnapshot("buffered_sequence_gap");
            return Result::ERROR_SEQUENCE_GAP;
        }
        if (apply_delta(delta) != Result::SUCCESS) {
            trigger_resnapshot("buffered_delta_apply_failed");
            return Result::ERROR_BOOK_CORRUPTED;
        }
    }

    buffered_applied_ += static_cast<uint64_t>(delta_buffer_.size() - start_index);
    buffered_skipped_ += static_cast<uint64_t>(skipped);
    LOG_INFO("[Binance] Applied buffered deltas", "applied", delta_buffer_.size() - start_index,
             "skipped", skipped, "buffer_high_water_mark", buffer_high_water_mark_);
    delta_buffer_.clear();
    return Result::SUCCESS;
}

auto BinanceFeedHandler::validate_delta_sequence(uint64_t first_update_id,
                                                 uint64_t last_update_id) const -> bool {
    uint64_t expected = last_sequence_.load(std::memory_order_acquire) + 1;
    return first_update_id <= expected && expected <= last_update_id;
}

void BinanceFeedHandler::trigger_resnapshot(const std::string& reason) {
    ++resync_count_;
    last_resync_reason_ = reason;
    LOG_WARN("[Binance] Triggering re-snapshot", "reason", reason.c_str(), "resync_count", resync_count_,
             "buffer_high_water_mark", buffer_high_water_mark_);
    if (error_callback_) {
        error_callback_("Re-snapshot: " + reason);
    }
    delta_buffer_.clear();
    state_.store(State::BUFFERING, std::memory_order_release);
    reconnect_requested_.store(true, std::memory_order_release);
}

}
