#pragma once

#include "../../common/types.hpp"
#include "../../common/logging.hpp"
#include "../../common/rest_client.hpp"
#include <string>
#include <vector>
#include <deque>
#include <functional>
#include <atomic>

namespace trading {

class BinanceFeedHandler {
public:
    using SnapshotCallback = std::function<void(const Snapshot&)>;
    using DeltaCallback = std::function<void(const Delta&)>;
    using ErrorCallback = std::function<void(const std::string&)>;

    explicit BinanceFeedHandler(
        const std::string& symbol,
        const std::string& api_key = "",
        const std::string& api_secret = "",
        const std::string& api_url = "https://api.binance.com",
        const std::string& ws_url = "wss://stream.binance.com:9443/ws"
    );

    ~BinanceFeedHandler();

    BinanceFeedHandler(const BinanceFeedHandler&) = delete;
    BinanceFeedHandler& operator=(const BinanceFeedHandler&) = delete;

    static std::string get_api_key_from_env();
    static std::string get_api_secret_from_env();

    void set_snapshot_callback(SnapshotCallback callback) {
        snapshot_callback_ = std::move(callback);
    }

    void set_delta_callback(DeltaCallback callback) {
        delta_callback_ = std::move(callback);
    }

    void set_error_callback(ErrorCallback callback) {
        error_callback_ = std::move(callback);
    }

    Result start();
    void stop();

    bool is_running() const {
        return running_.load(std::memory_order_acquire);
    }

    uint64_t get_sequence() const {
        return last_update_id_.load(std::memory_order_acquire);
    }

    Result process_message(const std::string& message);

private:
    enum class State {
        DISCONNECTED,
        BUFFERING,
        SYNCHRONIZED,
        STREAMING
    };

    std::string symbol_;
    std::string api_key_;
    std::string api_secret_;
    std::string api_url_;
    std::string ws_url_;

    std::atomic<bool> running_{false};
    std::atomic<uint64_t> last_update_id_{0};
    std::atomic<State> state_{State::DISCONNECTED};

    std::deque<std::string> delta_buffer_;
    static constexpr size_t MAX_BUFFER_SIZE = 1000;

    SnapshotCallback snapshot_callback_;
    DeltaCallback delta_callback_;
    ErrorCallback error_callback_;

    Result fetch_snapshot();
    Result connect_websocket();
    Result process_delta(const std::string& message);
    Result apply_buffered_deltas();
    bool validate_delta_sequence(uint64_t first_update_id, uint64_t last_update_id);
    void trigger_resnapshot(const std::string& reason);

    std::vector<PriceLevel> parse_price_levels(const std::string& json, const std::string& key);
};

}  // namespace trading
