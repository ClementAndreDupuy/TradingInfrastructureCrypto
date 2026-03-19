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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace trading {

// Kraken WebSocket v2 feed handler.
//
// Snapshot:  REST GET /0/public/Depth?pair={symbol}&count=500
//            Price levels are 3-element arrays: ["price","vol",timestamp]
//            No sequence number in REST response; last_seq_ starts at 0.
//
// Deltas:    WebSocket v2 (wss://ws.kraken.com/v2), channel "book"
//            Message format: {"channel":"book","type":"update","seq":N,"data":[...]}
//            Price levels: {"price":X,"qty":Y}
//            Sequence: strictly +1 per message (simpler than Binance's window rule)
class KrakenFeedHandler {
  public:
    using SnapshotCallback = std::function<void(const Snapshot&)>;
    using DeltaCallback = std::function<void(const Delta&)>;
    using ErrorCallback = std::function<void(const std::string&)>;

    explicit KrakenFeedHandler(const std::string& symbol, const std::string& api_key = "",
                               const std::string& api_secret = "",
                               const std::string& api_url = "https://api.kraken.com",
                               const std::string& ws_url = "wss://ws.kraken.com/v2");

    ~KrakenFeedHandler();

    KrakenFeedHandler(const KrakenFeedHandler&) = delete;
    KrakenFeedHandler& operator=(const KrakenFeedHandler&) = delete;

    static std::string get_api_key_from_env();
    static std::string get_api_secret_from_env();

    void set_snapshot_callback(SnapshotCallback cb) { snapshot_callback_ = std::move(cb); }
    void set_delta_callback(DeltaCallback cb) { delta_callback_ = std::move(cb); }
    void set_error_callback(ErrorCallback cb) { error_callback_ = std::move(cb); }

    Result start();
    void stop();

    bool is_running() const { return running_.load(std::memory_order_acquire); }
    uint64_t get_sequence() const { return last_seq_.load(std::memory_order_acquire); }
    double tick_size() const noexcept { return tick_size_; }

    Result process_message(const std::string& message);

  private:
    Result fetch_tick_size();
    enum class State { DISCONNECTED, BUFFERING, SYNCHRONIZED, STREAMING };

    std::string symbol_;
    std::string api_key_;
    std::string api_secret_;
    std::string api_url_;
    std::string ws_url_;
    VenueSymbols venue_symbols_;

    double tick_size_{0.0};

    std::atomic<bool> running_{false};
    std::atomic<uint64_t> last_seq_{0};
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
    Result fetch_snapshot();
    Result process_delta(const nlohmann::json& j, uint64_t seq);
    Result apply_buffered_deltas();
    // Kraken: seq must equal last_seq + 1 exactly (no window).
    bool validate_delta_sequence(uint64_t seq);
    void trigger_resnapshot(const std::string& reason);
};

} // namespace trading
