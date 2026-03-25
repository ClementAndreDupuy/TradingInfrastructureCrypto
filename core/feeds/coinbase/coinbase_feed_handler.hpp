#pragma once

#include "../../common/logging.hpp"
#include "../../common/types.hpp"
#include "../../common/symbol_mapper.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace trading {
    class CoinbaseFeedHandler {
    public:
        using SnapshotCallback = std::function<void(const Snapshot &)>;
        using DeltaCallback = std::function<void(const Delta &)>;
        using ErrorCallback = std::function<void(const std::string &)>;

        explicit CoinbaseFeedHandler(
            const std::string &symbol,
            const std::string &ws_url,
            const std::string &api_url);

        ~CoinbaseFeedHandler();

        CoinbaseFeedHandler(const CoinbaseFeedHandler &) = delete;

        CoinbaseFeedHandler &operator=(const CoinbaseFeedHandler &) = delete;

        void set_snapshot_callback(SnapshotCallback cb) { snapshot_callback_ = std::move(cb); }
        void set_delta_callback(DeltaCallback cb) { delta_callback_ = std::move(cb); }
        void set_error_callback(ErrorCallback cb) { error_callback_ = std::move(cb); }

        Result refresh_tick_size() { return fetch_tick_size(); }

        Result start();

        void stop();

        bool is_running() const { return running_.load(std::memory_order_acquire); }
        uint64_t get_sequence() const { return last_sequence_.load(std::memory_order_acquire); }
        double tick_size() const noexcept { return tick_size_; }

        Result process_message(const std::string &message);

        std::vector<std::string> build_subscription_messages();

        static std::string coinbase_api_key_from_env();

        static std::string coinbase_api_secret_from_env();

        static std::string generate_jwt(const std::string &api_key, const std::string &api_secret);

    private:
        Result fetch_tick_size();

        enum class State { DISCONNECTED, BUFFERING, STREAMING };

        std::string symbol_;
        std::string ws_url_;
        std::string api_url_;
        double tick_size_{0.0};
        VenueSymbols venue_symbols_;

        std::atomic<bool> running_{false};
        std::atomic<uint64_t> last_sequence_{0};
        std::atomic<State> state_{State::DISCONNECTED};
        std::atomic<void *> lws_ctx_{nullptr};
        std::atomic<bool> reconnect_requested_{false};
        std::atomic<int64_t> last_heartbeat_ns_{0};
        std::atomic<int64_t> last_event_time_exchange_ns_{0};
        std::atomic<bool> auth_rejected_{false};
        std::atomic<bool> subscription_rejected_{false};

        std::thread ws_thread_;
        std::mutex ws_mutex_;
        std::condition_variable ws_cv_;
        std::string last_start_failure_reason_;

        std::deque<std::string> delta_buffer_;
        static constexpr size_t MAX_BUFFER_SIZE = 1000;

        SnapshotCallback snapshot_callback_;
        DeltaCallback delta_callback_;
        ErrorCallback error_callback_;

        void ws_event_loop();

        int64_t extract_exchange_timestamp_ns(const nlohmann::json &j) const;

        Result process_snapshot(const nlohmann::json &j, uint64_t seq);

        Result process_delta(const nlohmann::json &j, uint64_t seq);

        Result apply_buffered_deltas();

        bool validate_delta_sequence(uint64_t seq) const;

        void trigger_resnapshot(const std::string &reason);

        void emit_ops_event();
    };
}
