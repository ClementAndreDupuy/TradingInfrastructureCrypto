#pragma once

#include "../../common/types.hpp"
#include "../../ipc/lob_publisher.hpp"
#include "../../orderbook/orderbook.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <vector>

namespace trading {

// Connects any feed handler's callbacks to an OrderBook.
//
// Usage:
//   BookManager mgr("BTCUSDT", Exchange::BINANCE, /*tick=*/1.0, /*levels=*/20000);
//   handler.set_snapshot_callback(mgr.snapshot_handler());
//   handler.set_delta_callback(mgr.delta_handler());
//   handler.start();
//   // book is now live
//   std::cout << mgr.best_bid() << " / " << mgr.best_ask();
//
// Optional LOB feed for the neural model:
//   mgr.set_publisher(&lob_publisher);   // call before start()
//   // Every successful book update publishes a 5-level snapshot to the ring
//   // buffer so the Python shadow session can consume live multi-exchange data.
class BookManager {
  public:
    static constexpr size_t PUB_LEVELS = 5;

    BookManager(const std::string& symbol, Exchange exchange, double tick_size = 1.0,
                size_t max_levels = 20000)
        : book_(symbol, exchange, tick_size, max_levels), last_update_ns_(0) {
        // Pre-allocate reusable vectors for the publisher (no hot-path allocation)
        pub_bids_.reserve(PUB_LEVELS);
        pub_asks_.reserve(PUB_LEVELS);
    }

    // Attach a LobPublisher. Must be called before snapshot/delta handlers fire.
    // Pass nullptr to detach. Ownership remains with the caller.
    void set_publisher(LobPublisher* p) noexcept { publisher_ = p; }

    // Returns a lambda suitable for set_snapshot_callback().
    std::function<void(const Snapshot&)> snapshot_handler() {
        return [this](const Snapshot& s) {
            auto result = book_.apply_snapshot(s);
            if (result != Result::SUCCESS) {
                LOG_ERROR("BookManager: snapshot apply failed", "symbol", book_.symbol().c_str(),
                          "exchange", exchange_to_string(book_.exchange()));
            } else {
                last_update_ns_.store(now_ns(), std::memory_order_release);
                publish_lob(s.timestamp_local_ns);
            }
        };
    }

    // Returns a lambda suitable for set_delta_callback().
    std::function<void(const Delta&)> delta_handler() {
        return [this](const Delta& d) {
            auto result = book_.apply_delta(d);
            if (result == Result::ERROR_INVALID_PRICE) {
                LOG_WARN("BookManager: delta out of grid range — re-snapshot needed", "symbol",
                         book_.symbol().c_str(), "price", d.price);
            } else {
                last_update_ns_.store(now_ns(), std::memory_order_release);
                publish_lob(d.timestamp_local_ns);
            }
        };
    }

    double best_bid() const { return book_.get_best_bid(); }
    double best_ask() const { return book_.get_best_ask(); }
    double mid_price() const { return book_.get_mid_price(); }
    double spread() const { return book_.get_spread(); }
    bool is_ready() const { return book_.is_initialized(); }

    // Milliseconds since the last successful snapshot or delta.
    // Returns INT64_MAX if the book has never been updated.
    int64_t age_ms() const noexcept {
        int64_t ts = last_update_ns_.load(std::memory_order_acquire);
        if (ts == 0)
            return INT64_MAX;
        return (now_ns() - ts) / 1'000'000LL;
    }

    void get_top_levels(size_t n, std::vector<PriceLevel>& bids,
                        std::vector<PriceLevel>& asks) const {
        book_.get_top_levels(n, bids, asks);
    }

    const OrderBook& book() const { return book_; }

  private:
    OrderBook book_;
    std::atomic<int64_t> last_update_ns_;
    LobPublisher* publisher_ = nullptr;

    // Pre-allocated; reused every publish call — no heap allocation in hot path.
    std::vector<PriceLevel> pub_bids_;
    std::vector<PriceLevel> pub_asks_;

    void publish_lob(int64_t timestamp_ns) noexcept {
        if (!publisher_ || !publisher_->is_open()) return;
        pub_bids_.clear();
        pub_asks_.clear();
        book_.get_top_levels(PUB_LEVELS, pub_bids_, pub_asks_);
        publisher_->publish(
            book_.exchange(), book_.symbol().c_str(),
            timestamp_ns, book_.get_mid_price(),
            pub_bids_, pub_asks_);
    }

    static int64_t now_ns() noexcept {
        using namespace std::chrono;
        return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    }
};

} // namespace trading
