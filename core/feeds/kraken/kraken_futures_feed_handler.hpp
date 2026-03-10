#pragma once

// Kraken Futures (formerly Cryptofacilities) WebSocket v1 feed handler.
//
// Endpoint: wss://futures.kraken.com/ws/v1
//
// Subscribe: {"event":"subscribe","feed":"book","product_ids":["PI_XBTUSD"]}
//
// Snapshot:  {"feed":"book_snapshot","product_id":"PI_XBTUSD","seq":1,
//             "bids":[{"price":X,"qty":Y},...], "asks":[...]}
//
// Update:    {"feed":"book","product_id":"PI_XBTUSD","seq":N,
//             "side":"sell","price":X,"qty":Y,"timestamp":T}
//            One level per message (unlike spot which batches all levels).
//
// Sequence: Strictly +1; gap triggers resubscription.

#include "../../common/types.hpp"
#include "../../common/logging.hpp"
#include "../../common/rest_client.hpp"
#include <string>
#include <vector>
#include <deque>
#include <functional>
#include <atomic>
#include <cstring>

namespace trading {

class KrakenFuturesFeedHandler {
public:
    using SnapshotCallback = std::function<void(const Snapshot&)>;
    using DeltaCallback    = std::function<void(const Delta&)>;
    using ErrorCallback    = std::function<void(const std::string&)>;

    explicit KrakenFuturesFeedHandler(
        const std::string& product_id,
        const std::string& api_key    = "",
        const std::string& api_secret = "",
        const std::string& ws_url     = "wss://futures.kraken.com/ws/v1"
    ) : product_id_(product_id), api_key_(api_key), api_secret_(api_secret), ws_url_(ws_url) {}

    ~KrakenFuturesFeedHandler() { stop(); }

    KrakenFuturesFeedHandler(const KrakenFuturesFeedHandler&) = delete;
    KrakenFuturesFeedHandler& operator=(const KrakenFuturesFeedHandler&) = delete;

    void set_snapshot_callback(SnapshotCallback cb) { snapshot_callback_ = std::move(cb); }
    void set_delta_callback(DeltaCallback cb)        { delta_callback_    = std::move(cb); }
    void set_error_callback(ErrorCallback cb)        { error_callback_    = std::move(cb); }

    Result start() {
        if (running_.load(std::memory_order_acquire)) return Result::SUCCESS;
        running_.store(true, std::memory_order_release);
        state_.store(State::WAITING_SNAPSHOT, std::memory_order_release);
        return connect_websocket();
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        state_.store(State::DISCONNECTED, std::memory_order_release);
        delta_buffer_.clear();
    }

    bool     is_running()    const { return running_.load(std::memory_order_acquire); }
    uint64_t get_sequence()  const { return last_seq_.load(std::memory_order_acquire); }

    // Entry point for incoming WebSocket messages (event-loop → here).
    Result process_message(const std::string& msg) {
        if (msg.empty()) return Result::SUCCESS;

        // Heartbeat
        if (msg.find("\"feed\":\"heartbeat\"") != std::string::npos) return Result::SUCCESS;

        // Connection / challenge acknowledgement
        if (msg.find("\"event\":\"subscribed\"") != std::string::npos ||
            msg.find("\"event\":\"info\"")       != std::string::npos) return Result::SUCCESS;

        // Error from exchange
        if (msg.find("\"event\":\"error\"") != std::string::npos) {
            LOG_ERROR("Kraken Futures WS error", "msg", msg.c_str());
            if (error_callback_) error_callback_(msg);
            return Result::ERROR_CONNECTION_LOST;
        }

        // Snapshot
        if (msg.find("\"feed\":\"book_snapshot\"") != std::string::npos)
            return process_snapshot(msg);

        // Delta
        if (msg.find("\"feed\":\"book\"") != std::string::npos)
            return process_delta(msg);

        return Result::SUCCESS;
    }

private:
    enum class State { DISCONNECTED, WAITING_SNAPSHOT, STREAMING };

    std::string           product_id_;
    std::string           api_key_;
    std::string           api_secret_;
    std::string           ws_url_;

    std::atomic<bool>     running_{false};
    std::atomic<uint64_t> last_seq_{0};
    std::atomic<State>    state_{State::DISCONNECTED};

    std::deque<std::string> delta_buffer_;
    static constexpr size_t MAX_BUFFER_SIZE = 1000;

    SnapshotCallback snapshot_callback_;
    DeltaCallback    delta_callback_;
    ErrorCallback    error_callback_;

    // ── Helpers ──────────────────────────────────────────────────────────────

    // Minimal JSON field extractor. Returns empty string if not found.
    static std::string json_get_str(const std::string& json, const std::string& key) {
        std::string search = "\"" + key + "\":";
        auto pos = json.find(search);
        if (pos == std::string::npos) return "";
        pos += search.size();
        // Skip whitespace
        while (pos < json.size() && json[pos] == ' ') ++pos;
        if (pos >= json.size()) return "";
        if (json[pos] == '"') {
            ++pos;
            auto end = json.find('"', pos);
            if (end == std::string::npos) return "";
            return json.substr(pos, end - pos);
        }
        // Numeric: read until delimiter
        auto end = json.find_first_of(",}", pos);
        if (end == std::string::npos) end = json.size();
        return json.substr(pos, end - pos);
    }

    static double json_get_double(const std::string& json, const std::string& key, double def = 0.0) {
        std::string s = json_get_str(json, key);
        if (s.empty()) return def;
        try { return std::stod(s); } catch (...) { return def; }
    }

    static uint64_t json_get_uint64(const std::string& json, const std::string& key, uint64_t def = 0) {
        std::string s = json_get_str(json, key);
        if (s.empty()) return def;
        try { return std::stoull(s); } catch (...) { return def; }
    }

    // Parse array of {"price":X,"qty":Y} objects
    std::vector<PriceLevel> parse_levels(const std::string& json, const std::string& array_key) {
        std::vector<PriceLevel> levels;
        std::string search = "\"" + array_key + "\":[";
        auto array_start = json.find(search);
        if (array_start == std::string::npos) return levels;
        array_start += search.size();

        size_t depth = 1;
        size_t pos = array_start;
        while (pos < json.size() && depth > 0) {
            if (json[pos] == '{') {
                // Find matching }
                size_t obj_start = pos;
                size_t obj_depth = 1;
                ++pos;
                while (pos < json.size() && obj_depth > 0) {
                    if (json[pos] == '{') ++obj_depth;
                    else if (json[pos] == '}') --obj_depth;
                    ++pos;
                }
                std::string obj = json.substr(obj_start, pos - obj_start);
                double price = json_get_double(obj, "price");
                double qty   = json_get_double(obj, "qty");
                if (price > 0.0) levels.push_back({price, qty});
            } else if (json[pos] == '[') { ++depth; ++pos; }
            else if (json[pos] == ']') { --depth; ++pos; }
            else { ++pos; }
        }
        return levels;
    }

    // ── Core processing ──────────────────────────────────────────────────────

    Result connect_websocket() {
        // Send subscription request. Actual WebSocket I/O handled by the event loop
        // that calls process_message(). See perp_arb_main.cpp for integration.
        LOG_INFO("Kraken Futures: connecting", "url", ws_url_.c_str(),
                 "product_id", product_id_.c_str());

        // Subscription message to be sent by the event loop after connection:
        // {"event":"subscribe","feed":"book","product_ids":["PI_XBTUSD"]}
        return Result::SUCCESS;
    }

    Result process_snapshot(const std::string& msg) {
        uint64_t seq = json_get_uint64(msg, "seq");

        Snapshot snap;
        snap.symbol             = product_id_;
        snap.exchange           = Exchange::KRAKEN;
        snap.sequence           = seq;
        snap.timestamp_local_ns = now_ns();
        snap.timestamp_exchange_ns = snap.timestamp_local_ns;

        snap.bids = parse_levels(msg, "bids");
        snap.asks = parse_levels(msg, "asks");

        if (snap.bids.empty() && snap.asks.empty()) {
            LOG_WARN("Kraken Futures: empty snapshot", "product_id", product_id_.c_str());
            return Result::ERROR_INVALID_PRICE;
        }

        last_seq_.store(seq, std::memory_order_release);
        state_.store(State::STREAMING, std::memory_order_release);

        LOG_INFO("Kraken Futures: snapshot applied",
                 "product_id", product_id_.c_str(),
                 "seq", seq,
                 "bids", snap.bids.size(),
                 "asks", snap.asks.size());

        if (snapshot_callback_) snapshot_callback_(snap);

        // Drain any buffered deltas that arrived before the snapshot
        apply_buffered_deltas();
        return Result::SUCCESS;
    }

    Result process_delta(const std::string& msg) {
        State cur_state = state_.load(std::memory_order_acquire);

        uint64_t seq  = json_get_uint64(msg, "seq");
        std::string side_str = json_get_str(msg, "side");
        double price = json_get_double(msg, "price");
        double qty   = json_get_double(msg, "qty");
        double ts    = json_get_double(msg, "timestamp");

        if (price <= 0.0) return Result::SUCCESS;  // spurious

        if (cur_state == State::WAITING_SNAPSHOT) {
            if (delta_buffer_.size() < MAX_BUFFER_SIZE) {
                delta_buffer_.push_back(msg);
            }
            return Result::SUCCESS;
        }

        if (!validate_delta_sequence(seq)) {
            trigger_resubscribe("sequence gap");
            return Result::ERROR_SEQUENCE_GAP;
        }

        Delta d;
        d.side                 = (side_str == "buy") ? Side::BID : Side::ASK;
        d.price                = price;
        d.size                 = qty;
        d.sequence             = seq;
        d.timestamp_local_ns   = now_ns();
        d.timestamp_exchange_ns = (ts > 0.0) ? static_cast<int64_t>(ts * 1e6) : d.timestamp_local_ns;

        last_seq_.store(seq, std::memory_order_release);
        if (delta_callback_) delta_callback_(d);
        return Result::SUCCESS;
    }

    void apply_buffered_deltas() {
        while (!delta_buffer_.empty()) {
            process_delta(delta_buffer_.front());
            delta_buffer_.pop_front();
        }
    }

    bool validate_delta_sequence(uint64_t seq) noexcept {
        uint64_t expected = last_seq_.load(std::memory_order_acquire) + 1;
        return (seq == expected);
    }

    void trigger_resubscribe(const std::string& reason) {
        LOG_WARN("Kraken Futures: resubscribing", "reason", reason.c_str(),
                 "product_id", product_id_.c_str());
        state_.store(State::WAITING_SNAPSHOT, std::memory_order_release);
        last_seq_.store(0, std::memory_order_release);
        delta_buffer_.clear();
        if (error_callback_) error_callback_("resubscribe:" + reason);
    }

    static int64_t now_ns() noexcept {
        using namespace std::chrono;
        return duration_cast<nanoseconds>(
            high_resolution_clock::now().time_since_epoch()).count();
    }
};

}  // namespace trading
