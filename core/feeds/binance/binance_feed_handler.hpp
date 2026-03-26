#pragma once

#include "../../common/logging.hpp"
#include "../../common/rest_client.hpp"
#include "../../common/types.hpp"
#include "../../common/symbol_mapper.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace trading {
    class BinanceFeedHandler {
    public:
        using SnapshotCallback = std::function<void(const Snapshot &)>;
        using DeltaCallback = std::function<void(const Delta &)>;
        using ErrorCallback = std::function<void(const std::string &)>;

        enum class State { DISCONNECTED, BUFFERING, STREAMING };

        struct ParsedLevel {
            double price{0.0};
            double size{0.0};
        };

        struct BufferedDelta {
            uint64_t first_update_id{0};
            uint64_t last_update_id{0};
            int64_t timestamp_exchange_ns{0};
            std::vector<ParsedLevel> bids;
            std::vector<ParsedLevel> asks;
        };

        static constexpr size_t MAX_BUFFER_SIZE = 8192;

        struct SyncStats {
            uint64_t resync_count{0};
            uint64_t snapshot_latency_ms{0};
            uint64_t buffered_applied{0};
            uint64_t buffered_skipped{0};
            size_t buffer_high_water_mark{0};
            std::string last_resync_reason;
        };

        explicit BinanceFeedHandler(const std::string &symbol,
                                    const std::string &api_url,
                                    const std::string &ws_url);

        ~BinanceFeedHandler();

        BinanceFeedHandler(const BinanceFeedHandler &) = delete;

        BinanceFeedHandler &operator=(const BinanceFeedHandler &) = delete;

        void set_snapshot_callback(SnapshotCallback cb) { snapshot_callback_ = std::move(cb); }
        void set_delta_callback(DeltaCallback cb) { delta_callback_ = std::move(cb); }
        void set_error_callback(ErrorCallback cb) { error_callback_ = std::move(cb); }

        Result refresh_tick_size() { return fetch_tick_size(); }

        Result start();

        void stop();

        bool is_running() const { return running_.load(std::memory_order_acquire); }
        uint64_t get_sequence() const { return last_sequence_.load(std::memory_order_acquire); }
        double tick_size() const noexcept { return tick_size_; }

        SyncStats sync_stats() const;

        Result process_message(const std::string &message);

        Result apply_buffered_deltas(uint64_t snapshot_sequence);

        // Test injection interface
        void set_state(State s) { state_.store(s, std::memory_order_release); }
        void set_last_sequence(uint64_t n) { last_sequence_.store(n, std::memory_order_release); }
        bool reconnect_requested() const { return reconnect_requested_.load(std::memory_order_acquire); }
        std::vector<BufferedDelta> &delta_buffer() { return delta_buffer_; }
        const std::vector<BufferedDelta> &delta_buffer() const { return delta_buffer_; }

    private:
        Result fetch_tick_size();

        Result parse_delta_message(const nlohmann::json &json, BufferedDelta &delta) const;

        Result process_snapshot();

        Result apply_delta(const BufferedDelta &delta);

        bool validate_delta_sequence(uint64_t first_update_id, uint64_t last_update_id) const;

        void trigger_resnapshot(const std::string &reason);

        void ws_event_loop();

        std::string symbol_;
        std::string api_url_;
        std::string ws_url_;
        VenueSymbols venue_symbols_;

        double tick_size_{0.0};

        std::atomic<bool> running_{false};
        std::atomic<bool> reconnect_requested_{false};
        std::atomic<uint64_t> last_sequence_{0};
        std::atomic<State> state_{State::DISCONNECTED};
        std::atomic<void *> lws_ctx_{nullptr};

        std::thread ws_thread_;
        std::mutex ws_mutex_;
        std::condition_variable ws_cv_;

        std::vector<BufferedDelta> delta_buffer_;
        size_t buffer_high_water_mark_{0};
        uint64_t buffered_applied_{0};
        uint64_t buffered_skipped_{0};
        uint64_t resync_count_{0};
        uint64_t snapshot_latency_ms_{0};
        std::string last_resync_reason_;

        SnapshotCallback snapshot_callback_;
        DeltaCallback delta_callback_;
        ErrorCallback error_callback_;

        std::chrono::steady_clock::time_point last_snapshot_time_{};
    };
}
