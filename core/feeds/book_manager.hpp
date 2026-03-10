#pragma once

#include "../orderbook/orderbook.hpp"
#include "../common/types.hpp"
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
class BookManager {
public:
    BookManager(const std::string& symbol, Exchange exchange,
                double tick_size = 1.0, size_t max_levels = 20000)
        : book_(symbol, exchange, tick_size, max_levels) {}

    // Returns a lambda suitable for set_snapshot_callback().
    std::function<void(const Snapshot&)> snapshot_handler() {
        return [this](const Snapshot& s) {
            auto result = book_.apply_snapshot(s);
            if (result != Result::SUCCESS) {
                LOG_ERROR("BookManager: snapshot apply failed",
                          "symbol", book_.symbol().c_str(),
                          "exchange", exchange_to_string(book_.exchange()));
            }
        };
    }

    // Returns a lambda suitable for set_delta_callback().
    std::function<void(const Delta&)> delta_handler() {
        return [this](const Delta& d) {
            auto result = book_.apply_delta(d);
            if (result == Result::ERROR_INVALID_PRICE) {
                LOG_WARN("BookManager: delta out of grid range — re-snapshot needed",
                         "symbol", book_.symbol().c_str(),
                         "price", d.price);
            }
        };
    }

    double best_bid()  const { return book_.get_best_bid(); }
    double best_ask()  const { return book_.get_best_ask(); }
    double mid_price() const { return book_.get_mid_price(); }
    double spread()    const { return book_.get_spread(); }
    bool   is_ready()  const { return book_.is_initialized(); }

    void get_top_levels(size_t n,
                        std::vector<PriceLevel>& bids,
                        std::vector<PriceLevel>& asks) const {
        book_.get_top_levels(n, bids, asks);
    }

    const OrderBook& book() const { return book_; }

private:
    OrderBook book_;
};

}  // namespace trading
