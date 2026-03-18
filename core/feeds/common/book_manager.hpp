#pragma once

#include "../../common/types.hpp"
#include "../../ipc/lob_publisher.hpp"
#include "../../orderbook/orderbook.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <vector>

namespace trading {

class BookManager {
  public:
    BookManager(const std::string& symbol, Exchange exchange, double tick_size = 1.0,
                size_t max_levels = 20000)
        : book_(symbol, exchange, tick_size, max_levels), last_update_steady_ns_(0),
          publisher_(nullptr) {
        pub_bids_.reserve(5);
        pub_asks_.reserve(5);
    }

    void set_publisher(LobPublisher* publisher) noexcept { publisher_ = publisher; }

    std::function<void(const Snapshot&)> snapshot_handler() {
        return [this](const Snapshot& s) {
            auto result = book_.apply_snapshot(s);
            if (result != Result::SUCCESS) {
                LOG_ERROR("BookManager: snapshot apply failed", "symbol", book_.symbol().c_str(),
                          "exchange", exchange_to_string(book_.exchange()));
            } else {
                const int64_t wall_ts_ns =
                    (s.timestamp_local_ns > 0) ? s.timestamp_local_ns : wall_now_ns();
                last_update_steady_ns_.store(steady_now_ns(), std::memory_order_release);
                publish_lob(wall_ts_ns);
            }
        };
    }

    std::function<void(const Delta&)> delta_handler() {
        return [this](const Delta& d) {
            auto result = book_.apply_delta(d);
            if (result == Result::ERROR_INVALID_PRICE) {
                LOG_WARN("BookManager: delta out of grid range — re-snapshot needed", "symbol",
                         book_.symbol().c_str(), "price", d.price);
            } else if (result == Result::SUCCESS) {
                const int64_t wall_ts_ns =
                    (d.timestamp_local_ns > 0) ? d.timestamp_local_ns : wall_now_ns();
                last_update_steady_ns_.store(steady_now_ns(), std::memory_order_release);
                publish_lob(wall_ts_ns);
            }
        };
    }

    double best_bid() const { return book_.get_best_bid(); }
    double best_ask() const { return book_.get_best_ask(); }
    double mid_price() const { return book_.get_mid_price(); }
    double spread() const { return book_.get_spread(); }
    bool is_ready() const { return book_.is_initialized(); }

    int64_t age_ms() const noexcept {
        int64_t ts = last_update_steady_ns_.load(std::memory_order_acquire);
        if (ts == 0)
            return INT64_MAX;
        return (steady_now_ns() - ts) / 1'000'000LL;
    }

    void get_top_levels(size_t n, std::vector<PriceLevel>& bids,
                        std::vector<PriceLevel>& asks) const {
        book_.get_top_levels(n, bids, asks);
    }

    const OrderBook& book() const { return book_; }

  private:
    OrderBook book_;
    std::atomic<int64_t> last_update_steady_ns_;
    LobPublisher* publisher_;
    std::vector<PriceLevel> pub_bids_;
    std::vector<PriceLevel> pub_asks_;

    void publish_lob(int64_t timestamp_ns) {
        if (publisher_ == nullptr || !publisher_->is_open()) {
            return;
        }

        pub_bids_.clear();
        pub_asks_.clear();
        book_.get_top_levels(5, pub_bids_, pub_asks_);
        publisher_->publish(book_.exchange(), book_.symbol(), timestamp_ns, book_.get_mid_price(),
                            pub_bids_, pub_asks_);
    }

    static int64_t wall_now_ns() noexcept {
        using namespace std::chrono;
        return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
    }

    static int64_t steady_now_ns() noexcept {
        using namespace std::chrono;
        return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    }
};

} // namespace trading
