#pragma once

#include "../../common/logging.hpp"
#include "../../common/rest_client.hpp"
#include "../../common/types.hpp"
#include "../common/symbol_mapper.hpp"
#ifdef __has_include
#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
#else
#include "../../common/json.hpp"
#endif
#else
#include "../../common/json.hpp"
#endif
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace trading {

class OkxFeedHandler {
  public:
    using SnapshotCallback = std::function<void(const Snapshot&)>;
    using DeltaCallback = std::function<void(const Delta&)>;
    using ErrorCallback = std::function<void(const std::string&)>;

    explicit OkxFeedHandler(const std::string& symbol,
                            const std::string& api_url = "https://www.okx.com",
                            const std::string& ws_url = "wss://ws.okx.com:8443/ws/v5/public");

    ~OkxFeedHandler();

    OkxFeedHandler(const OkxFeedHandler&) = delete;
    OkxFeedHandler& operator=(const OkxFeedHandler&) = delete;

    void set_snapshot_callback(SnapshotCallback cb) { snapshot_callback_ = std::move(cb); }
    void set_delta_callback(DeltaCallback cb) { delta_callback_ = std::move(cb); }
    void set_error_callback(ErrorCallback cb) { error_callback_ = std::move(cb); }

    Result start();
    void stop();

    bool is_running() const { return running_.load(std::memory_order_acquire); }
    uint64_t get_sequence() const { return last_sequence_.load(std::memory_order_acquire); }

    Result process_message(const std::string& message);

    // Test hooks for deterministic unit coverage without touching internal symbols.
    void set_streaming_state_for_test(uint64_t last_sequence) {
        last_sequence_.store(last_sequence, std::memory_order_release);
        state_.store(State::STREAMING, std::memory_order_release);
    }

    void seed_book_state_for_test(const std::vector<PriceLevel>& bids,
                                  const std::vector<PriceLevel>& asks) {
        bids_.clear();
        asks_.clear();
        for (const auto& level : bids) {
            bids_[level.price] = {std::to_string(level.price), std::to_string(level.size)};
        }
        for (const auto& level : asks) {
            asks_[level.price] = {std::to_string(level.price), std::to_string(level.size)};
        }
    }

    // Exposes the internal CRC32 algorithm for test checksum construction.
    static uint32_t crc32_for_test(const std::string& data);

  private:
    enum class State { DISCONNECTED, BUFFERING, STREAMING };

    std::string symbol_;
    VenueSymbols venue_symbols_;
    std::string inst_id_;
    std::string api_url_;
    std::string ws_url_;

    std::atomic<bool> running_{false};
    std::atomic<bool> force_reconnect_{false};
    std::atomic<uint64_t> last_sequence_{0};
    std::atomic<State> state_{State::DISCONNECTED};
    std::atomic<void*> lws_ctx_{nullptr};

    std::thread ws_thread_;
    std::mutex ws_mutex_;
    std::condition_variable ws_cv_;

    std::deque<std::string> delta_buffer_;
    static constexpr size_t MAX_BUFFER_SIZE = 1000;

    SnapshotCallback snapshot_callback_;
    DeltaCallback delta_callback_;
    ErrorCallback error_callback_;

    void ws_event_loop();
    // Handles action:"snapshot" WS push — initialises the book, replays any buffered
    // updates that arrived before the snapshot, then transitions to STREAMING.
    Result process_ws_snapshot(const nlohmann::json& json);
    Result process_delta(const nlohmann::json& j, uint64_t seq);
    Result apply_buffered_deltas();
    bool validate_delta_sequence(uint64_t seq, uint64_t prev_seq) const;
    bool validate_checksum(const nlohmann::json& data) const;
    void apply_local_book_levels(const nlohmann::json& data);
    void trigger_resnapshot(const std::string& reason);

    // Key: price as double (for ordering/lookup).
    // Value: {price_str, size_str} — original wire strings used verbatim in CRC32.
    std::map<double, std::pair<std::string, std::string>, std::greater<double>> bids_;
    std::map<double, std::pair<std::string, std::string>> asks_;
};

} // namespace trading
