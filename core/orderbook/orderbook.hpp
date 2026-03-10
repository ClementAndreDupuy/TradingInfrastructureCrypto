#pragma once

#include "../common/types.hpp"
#include "../common/logging.hpp"
#include <vector>
#include <atomic>
#include <cmath>

namespace trading {

// Flat-array order book keyed by price tick grid.
//
// Price grid is centered on the best bid at snapshot time:
//   base_price = best_bid - (max_levels / 2) * tick_size
//   index      = round((price - base_price) / tick_size)
//
// This avoids the modulo-collision bug and gives O(1) updates
// with full cache locality. Pre-allocates all memory at construction.
//
// Sizing guidelines:
//   BTC/USD : tick_size=1.0,  max_levels=20000  → $20,000 range
//   ETH/USD : tick_size=0.10, max_levels=20000  → $2,000 range
//   SOL/USD : tick_size=0.01, max_levels=20000  → $200 range
class OrderBook {
public:
    OrderBook(const std::string& symbol, Exchange exchange,
              double tick_size = 1.0, size_t max_levels = 20000)
        : symbol_(symbol),
          exchange_(exchange),
          tick_size_(tick_size),
          max_levels_(max_levels),
          base_price_(0.0),
          sequence_(0),
          bid_sizes_(max_levels, 0.0),
          ask_sizes_(max_levels, 0.0) {}

    // Clears the book and re-centers the price grid around the snapshot's best bid.
    Result apply_snapshot(const Snapshot& snapshot) {
        if (snapshot.bids.empty()) {
            return Result::ERROR_INVALID_PRICE;
        }

        // Center grid: base = best_bid - half_range
        double best_bid = snapshot.bids[0].price;
        double new_base = best_bid - static_cast<double>(max_levels_ / 2) * tick_size_;

        // Reset state
        std::fill(bid_sizes_.begin(), bid_sizes_.end(), 0.0);
        std::fill(ask_sizes_.begin(), ask_sizes_.end(), 0.0);
        base_price_.store(new_base, std::memory_order_release);
        sequence_.store(snapshot.sequence, std::memory_order_release);

        size_t skipped = 0;
        for (const auto& level : snapshot.bids) {
            size_t idx;
            if (!price_to_index(level.price, idx)) { skipped++; continue; }
            bid_sizes_[idx] = level.size;
        }
        for (const auto& level : snapshot.asks) {
            size_t idx;
            if (!price_to_index(level.price, idx)) { skipped++; continue; }
            ask_sizes_[idx] = level.size;
        }

        if (skipped > 0) {
            LOG_WARN("Snapshot levels out of grid range", "symbol", symbol_.c_str(),
                     "skipped", skipped);
        }

        LOG_INFO("Snapshot applied", "symbol", symbol_.c_str(),
                 "sequence", snapshot.sequence,
                 "base_price", new_base,
                 "bids", snapshot.bids.size(),
                 "asks", snapshot.asks.size());
        return Result::SUCCESS;
    }

    Result apply_delta(const Delta& delta) {
        if (!is_initialized()) {
            return Result::ERROR_BOOK_CORRUPTED;
        }

        // Skip stale deltas (already reflected in a newer snapshot or earlier delta)
        if (delta.sequence > 0 && delta.sequence <= sequence_.load(std::memory_order_acquire)) {
            return Result::SUCCESS;
        }

        size_t idx;
        if (!price_to_index(delta.price, idx)) {
            LOG_WARN("Delta price out of grid range", "price", delta.price,
                     "base", base_price_.load(), "range", max_levels_ * tick_size_);
            return Result::ERROR_INVALID_PRICE;
        }

        if (delta.side == Side::BID) {
            bid_sizes_[idx] = delta.size;
        } else {
            ask_sizes_[idx] = delta.size;
        }

        if (delta.sequence > 0) {
            sequence_.store(delta.sequence, std::memory_order_release);
        }

        return Result::SUCCESS;
    }

    double get_best_bid() const {
        for (size_t i = max_levels_ - 1; i < max_levels_; --i) {
            if (bid_sizes_[i] > 0.0) return index_to_price(i);
        }
        return 0.0;
    }

    double get_best_ask() const {
        for (size_t i = 0; i < max_levels_; ++i) {
            if (ask_sizes_[i] > 0.0) return index_to_price(i);
        }
        return 0.0;
    }

    double get_mid_price() const {
        double bid = get_best_bid();
        double ask = get_best_ask();
        return (bid > 0.0 && ask > 0.0) ? (bid + ask) / 2.0 : 0.0;
    }

    double get_spread() const {
        return get_best_ask() - get_best_bid();
    }

    uint64_t get_sequence() const {
        return sequence_.load(std::memory_order_acquire);
    }

    bool is_initialized() const {
        return base_price_.load(std::memory_order_acquire) != 0.0;
    }

    void get_top_levels(size_t n,
                        std::vector<PriceLevel>& bids,
                        std::vector<PriceLevel>& asks) const {
        bids.clear();
        asks.clear();

        for (size_t i = max_levels_ - 1; i < max_levels_ && bids.size() < n; --i) {
            if (bid_sizes_[i] > 0.0) {
                bids.push_back(PriceLevel(index_to_price(i), bid_sizes_[i]));
            }
        }
        for (size_t i = 0; i < max_levels_ && asks.size() < n; ++i) {
            if (ask_sizes_[i] > 0.0) {
                asks.push_back(PriceLevel(index_to_price(i), ask_sizes_[i]));
            }
        }
    }

    double tick_size()   const { return tick_size_; }
    size_t max_levels()  const { return max_levels_; }
    double base_price()  const { return base_price_.load(std::memory_order_acquire); }
    const std::string& symbol()   const { return symbol_; }
    Exchange           exchange() const { return exchange_; }

private:
    std::string symbol_;
    Exchange    exchange_;
    double      tick_size_;
    size_t      max_levels_;

    std::atomic<double>    base_price_;
    std::atomic<uint64_t>  sequence_;

    std::vector<double> bid_sizes_;
    std::vector<double> ask_sizes_;

    // Maps price → flat-array index. Returns false if uninitialized or out of range.
    bool price_to_index(double price, size_t& out_idx) const {
        double base = base_price_.load(std::memory_order_acquire);
        if (base == 0.0) return false;
        double relative = (price - base) / tick_size_;
        int64_t idx = static_cast<int64_t>(relative + 0.5);  // round to nearest tick
        if (idx < 0 || static_cast<size_t>(idx) >= max_levels_) return false;
        out_idx = static_cast<size_t>(idx);
        return true;
    }

    double index_to_price(size_t idx) const {
        return base_price_.load(std::memory_order_acquire) +
               static_cast<double>(idx) * tick_size_;
    }
};

}  // namespace trading
