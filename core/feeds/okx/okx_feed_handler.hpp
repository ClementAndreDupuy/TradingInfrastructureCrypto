#pragma once

#include "../../common/logging.hpp"
#include "../../common/rest_client.hpp"
#include "../../common/types.hpp"
#include "../../common/symbol_mapper.hpp"
#include <nlohmann/json.hpp>
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
        using SnapshotCallback = std::function<void(const Snapshot &)>;
        using DeltaCallback = std::function<void(const Delta &)>;
        using TradeCallback = std::function<void(const TradeFlow &)>;
        using ErrorCallback = std::function<void(const std::string &)>;

        explicit OkxFeedHandler(const std::string &symbol,
                                const std::string &api_url,
                                const std::string &ws_url);

        ~OkxFeedHandler();

        OkxFeedHandler(const OkxFeedHandler &) = delete;

        OkxFeedHandler &operator=(const OkxFeedHandler &) = delete;

        void set_snapshot_callback(SnapshotCallback cb) { snapshot_callback_ = std::move(cb); }
        void set_delta_callback(DeltaCallback cb) { delta_callback_ = std::move(cb); }
        void set_trade_callback(TradeCallback cb) { trade_callback_ = std::move(cb); }
        void set_error_callback(ErrorCallback cb) { error_callback_ = std::move(cb); }

        Result refresh_tick_size() { return fetch_tick_size(); }

        Result start();

        void stop();

        bool is_running() const { return running_.load(std::memory_order_acquire); }
        uint64_t get_sequence() const { return last_sequence_.load(std::memory_order_acquire); }
        double tick_size() const noexcept { return tick_size_; }

        Result process_message(const std::string &message);

        void set_streaming_state_for_test(uint64_t last_sequence) {
            last_sequence_.store(last_sequence, std::memory_order_release);
            state_.store(State::STREAMING, std::memory_order_release);
        }

        void seed_book_state_for_test(const std::vector<PriceLevel> &bids,
                                      const std::vector<PriceLevel> &asks) {
            bids_.clear();
            asks_.clear();
            for (const auto &level: bids) {
                bids_[level.price] = {std::to_string(level.price), std::to_string(level.size)};
            }
            for (const auto &level: asks) {
                asks_[level.price] = {std::to_string(level.price), std::to_string(level.size)};
            }
        }

        static uint32_t crc32_for_test(const std::string &data);
        static constexpr size_t k_book_depth = 400;

    private:
        enum class State { DISCONNECTED, BUFFERING, STREAMING };

        std::string symbol_;
        VenueSymbols venue_symbols_;
        std::string instrument_id_;
        std::string api_url_;
        std::string ws_url_;
        double tick_size_{0.0};

        std::atomic<bool> running_{false};
        std::atomic<bool> force_reconnect_{false};
        std::atomic<uint64_t> last_sequence_{0};
        std::atomic<State> state_{State::DISCONNECTED};
        std::atomic<void *> lws_ctx_{nullptr};

        std::thread ws_thread_;
        std::mutex ws_mutex_;
        std::condition_variable ws_cv_;

        std::deque<std::string> delta_buffer_;
        static constexpr size_t MAX_BUFFER_SIZE = 1000;

        SnapshotCallback snapshot_callback_;
        DeltaCallback delta_callback_;
        TradeCallback trade_callback_;
        ErrorCallback error_callback_;

        Result fetch_tick_size();

        void ws_event_loop();

        Result process_snapshot(const nlohmann::json &json);

        Result process_delta(const nlohmann::json &j, uint64_t seq);

        Result apply_buffered_deltas();

        bool validate_delta_sequence(uint64_t seq, uint64_t prev_seq) const;

        bool validate_checksum(const nlohmann::json &data) const;

        void apply_local_book_levels(const nlohmann::json &data);

        void trigger_resnapshot(const std::string &reason);

        std::map<double, std::pair<std::string, std::string>, std::greater<double> > bids_;
        std::map<double, std::pair<std::string, std::string> > asks_;
    };
}
