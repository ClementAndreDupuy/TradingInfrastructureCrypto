#pragma once

#include "../../common/types.hpp"
#include "../../common/logging.hpp"
#include "../../common/rest_client.hpp"
#ifdef __has_include
#  if __has_include(<nlohmann/json.hpp>)
#    include <nlohmann/json.hpp>
#  else
#    include "../../common/json.hpp"
#  endif
#else
#  include "../../common/json.hpp"
#endif
#include <string>
#include <vector>
#include <deque>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <map>

namespace trading {

class OkxFeedHandler {
public:
    using SnapshotCallback = std::function<void(const Snapshot&)>;
    using DeltaCallback = std::function<void(const Delta&)>;
    using ErrorCallback = std::function<void(const std::string&)>;

    explicit OkxFeedHandler(
        const std::string& symbol,
        const std::string& api_url = "https://www.okx.com",
        const std::string& ws_url = "wss://ws.okx.com:8443/ws/v5/public"
    );

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

private:
    enum class State { DISCONNECTED, BUFFERING, STREAMING };

    std::string symbol_;
    std::string api_url_;
    std::string ws_url_;

    std::atomic<bool> running_{false};
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
    Result fetch_snapshot();
    Result process_delta(const nlohmann::json& j, uint64_t seq);
    Result apply_buffered_deltas();
    bool validate_delta_sequence(uint64_t seq, uint64_t prev_seq) const;
    bool validate_checksum(const nlohmann::json& data) const;
    void apply_local_book_levels(const nlohmann::json& data);
    void trigger_resnapshot(const std::string& reason);

    std::map<double, std::string, std::greater<double>> bids_;
    std::map<double, std::string> asks_;
};

}  // namespace trading
