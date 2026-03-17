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
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace trading {

class BinanceFeedHandler {
  public:
    using SnapshotCallback = std::function<void(const Snapshot&)>;
    using DeltaCallback = std::function<void(const Delta&)>;
    using ErrorCallback = std::function<void(const std::string&)>;

    explicit BinanceFeedHandler(const std::string& symbol, const std::string& api_key = "",
                                const std::string& api_secret = "",
                                const std::string& api_url = "https://api.binance.com",
                                const std::string& ws_url = "wss://stream.binance.com:9443/ws");

    ~BinanceFeedHandler();

    BinanceFeedHandler(const BinanceFeedHandler&) = delete;
    BinanceFeedHandler& operator=(const BinanceFeedHandler&) = delete;

    static std::string get_api_key_from_env();
    static std::string get_api_secret_from_env();

    void set_snapshot_callback(SnapshotCallback cb) { snapshot_callback_ = std::move(cb); }
    void set_delta_callback(DeltaCallback cb) { delta_callback_ = std::move(cb); }
    void set_error_callback(ErrorCallback cb) { error_callback_ = std::move(cb); }

    Result start();
    void stop();

    bool is_running() const { return running_.load(std::memory_order_acquire); }

    uint64_t get_sequence() const { return last_update_id_.load(std::memory_order_acquire); }

    Result process_message(const std::string& message);

  private:
    enum class State { DISCONNECTED, BUFFERING, SYNCHRONIZED, STREAMING };

    std::string symbol_;
    std::string api_key_;
    std::string api_secret_;
    std::string api_url_;
    std::string ws_url_;
    VenueSymbols venue_symbols_;

    std::atomic<bool> running_{false};
    std::atomic<uint64_t> last_update_id_{0};
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

    // Launched by start(); handles connect → snapshot → stream → reconnect loop.
    void ws_event_loop();
    Result fetch_snapshot();
    Result process_delta(const nlohmann::json& j, uint64_t seq);
    Result apply_buffered_deltas();
    bool validate_delta_sequence(uint64_t first_update_id, uint64_t last_update_id);
    void trigger_resnapshot(const std::string& reason);

    // Rate limit: enforces minimum 1s between REST snapshot calls.
    std::chrono::steady_clock::time_point last_snapshot_time_{};
};

} // namespace trading
