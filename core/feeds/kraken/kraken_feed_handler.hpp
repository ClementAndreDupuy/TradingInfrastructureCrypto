#pragma once

#include "../../common/types.hpp"
#include "../../common/logging.hpp"
#include "../../common/rest_client.hpp"
#include <string>
#include <vector>
#include <deque>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>

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
    using DeltaCallback    = std::function<void(const Delta&)>;
    using ErrorCallback    = std::function<void(const std::string&)>;

    explicit KrakenFeedHandler(
        const std::string& symbol,
        const std::string& api_key    = "",
        const std::string& api_secret = "",
        const std::string& api_url    = "https://api.kraken.com",
        const std::string& ws_url     = "wss://ws.kraken.com/v2"
    );

    ~KrakenFeedHandler();

    KrakenFeedHandler(const KrakenFeedHandler&)            = delete;
    KrakenFeedHandler& operator=(const KrakenFeedHandler&) = delete;

    static std::string get_api_key_from_env();
    static std::string get_api_secret_from_env();

    void set_snapshot_callback(SnapshotCallback cb) { snapshot_callback_ = std::move(cb); }
    void set_delta_callback(DeltaCallback cb)        { delta_callback_    = std::move(cb); }
    void set_error_callback(ErrorCallback cb)        { error_callback_    = std::move(cb); }

    Result start();
    void   stop();

    bool is_running() const { return running_.load(std::memory_order_acquire); }

    // Returns 0 after snapshot (Kraken REST has no sequence number).
    // Increments with each processed WebSocket message's seq field.
    uint64_t get_sequence() const { return last_seq_.load(std::memory_order_acquire); }

    Result process_message(const std::string& message);

private:
    enum class State { DISCONNECTED, BUFFERING, SYNCHRONIZED, STREAMING };

    std::string symbol_;
    std::string api_key_;
    std::string api_secret_;
    std::string api_url_;
    std::string ws_url_;

    std::atomic<bool>     running_{false};
    std::atomic<uint64_t> last_seq_{0};
    std::atomic<State>    state_{State::DISCONNECTED};
    std::atomic<void*>    lws_ctx_{nullptr};

    std::thread             ws_thread_;
    std::mutex              ws_mutex_;
    std::condition_variable ws_cv_;

    std::deque<std::string> delta_buffer_;
    static constexpr size_t MAX_BUFFER_SIZE = 1000;

    SnapshotCallback snapshot_callback_;
    DeltaCallback    delta_callback_;
    ErrorCallback    error_callback_;

    void   ws_event_loop();
    Result fetch_snapshot();
    Result process_delta(const std::string& message);
    Result apply_buffered_deltas();
    // Kraken: seq must equal last_seq + 1 exactly (no window).
    bool   validate_delta_sequence(uint64_t seq);
    void   trigger_resnapshot(const std::string& reason);

    // Parse Kraken REST depth levels: ["price_str","vol_str",timestamp_int]
    std::vector<PriceLevel> parse_kraken_rest_levels(const std::string& json,
                                                     const std::string& key);
    // Parse Kraken WebSocket v2 levels: {"price":X,"qty":Y}
    std::vector<PriceLevel> parse_kraken_ws_levels(const std::string& json,
                                                   const std::string& key);
};

}  // namespace trading
