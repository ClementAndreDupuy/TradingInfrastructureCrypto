#pragma once

#include "../common/logging.hpp"
#include "../common/types.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

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
    OrderBook(const std::string& symbol, Exchange exchange, double tick_size = 1.0,
              size_t max_levels = 20000)
        : symbol_(symbol), exchange_(exchange), tick_size_(tick_size), max_levels_(max_levels),
          base_price_(0.0), sequence_(0), initialized_(false), bid_sizes_(max_levels, 0.0),
          ask_sizes_(max_levels, 0.0), scratch_bid_sizes_(max_levels, 0.0),
          scratch_ask_sizes_(max_levels, 0.0), out_of_range_streak_(0) {}

    // Clears the book and re-centers the price grid around the snapshot's best bid.
    Result apply_snapshot(const Snapshot& snapshot) {
        if (snapshot.bids.empty() || snapshot.asks.empty()) {
            return Result::ERROR_INVALID_PRICE;
        }

        const double best_bid = snapshot.bids[0].price;
        const double best_ask = snapshot.asks[0].price;
        const double spread = best_ask - best_bid;
        if (spread <= 0.0) {
            LOG_ERROR("Invalid snapshot spread", "symbol", symbol_.c_str(), "best_bid", best_bid,
                      "best_ask", best_ask);
            return Result::ERROR_INVALID_PRICE;
        }

        const double max_allowed_spread = max_allowed_spread_abs();
        if (spread > max_allowed_spread) {
            LOG_ERROR("Snapshot spread too wide", "symbol", symbol_.c_str(), "spread", spread,
                      "max_allowed", max_allowed_spread, "best_bid", best_bid, "best_ask",
                      best_ask);
            return Result::ERROR_INVALID_PRICE;
        }

        if (snapshot.checksum_present) {
            const uint32_t local_checksum = compute_snapshot_checksum(snapshot);
            if (local_checksum != snapshot.checksum) {
                LOG_ERROR("Snapshot checksum mismatch", "symbol", symbol_.c_str(), "local",
                          local_checksum, "remote", snapshot.checksum);
                return Result::ERROR_BOOK_CORRUPTED;
            }
        }

        // Center grid: base = best_bid - half_range
        double new_base = best_bid - static_cast<double>(max_levels_ / 2) * tick_size_;

        // Reset state — mark uninitialized first so concurrent readers see a clean boundary
        // (either fully old book or fully new book, never a half-cleared one).
        // base_price_ and initialized_ are then updated together before repopulating,
        // because price_to_index() requires initialized_ == true to map levels.
        initialized_.store(false, std::memory_order_release);
        std::fill(bid_sizes_.begin(), bid_sizes_.end(), 0.0);
        std::fill(ask_sizes_.begin(), ask_sizes_.end(), 0.0);
        base_price_.store(new_base, std::memory_order_release);
        sequence_.store(snapshot.sequence, std::memory_order_release);
        out_of_range_streak_ = 0;
        // Re-enable after base_price_ is committed so price_to_index() works during populate.
        initialized_.store(true, std::memory_order_release);

        size_t skipped = 0;
        for (const auto& level : snapshot.bids) {
            size_t idx;
            if (!price_to_index(level.price, idx)) {
                skipped++;
                continue;
            }
            bid_sizes_[idx] = level.size;
        }
        for (const auto& level : snapshot.asks) {
            size_t idx;
            if (!price_to_index(level.price, idx)) {
                skipped++;
                continue;
            }
            ask_sizes_[idx] = level.size;
        }

        if (skipped > 0) {
            LOG_WARN("Snapshot levels out of grid range", "symbol", symbol_.c_str(), "skipped",
                     skipped);
        }

        LOG_INFO("Snapshot applied", "symbol", symbol_.c_str(), "sequence", snapshot.sequence,
                 "base_price", new_base, "bids", snapshot.bids.size(), "asks",
                 snapshot.asks.size());
        return Result::SUCCESS;
    }

    Result apply_delta(const Delta& delta) {
        if (!is_initialized()) {
            return Result::ERROR_BOOK_CORRUPTED;
        }

        if (delta.size < 0.0) {
            return Result::ERROR_INVALID_SIZE;
        }

        // Skip stale deltas (already reflected in a newer snapshot or earlier delta)
        if (delta.sequence > 0 && delta.sequence <= sequence_.load(std::memory_order_acquire)) {
            return Result::SUCCESS;
        }

        size_t idx;
        if (!price_to_index(delta.price, idx)) {
            if (should_recenter(delta.price)) {
                recenter_grid(delta.price);
                if (!price_to_index(delta.price, idx)) {
                    LOG_WARN("Delta price still out of recentered grid", "price", delta.price,
                             "base", base_price_.load(), "range", max_levels_ * tick_size_);
                    return Result::ERROR_INVALID_PRICE;
                }
            } else {
                LOG_WARN("Delta price out of grid range", "price", delta.price, "base",
                         base_price_.load(), "range", max_levels_ * tick_size_, "streak",
                         out_of_range_streak_);
                return Result::ERROR_INVALID_PRICE;
            }
        }

        out_of_range_streak_ = 0;

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
            if (bid_sizes_[i] > 0.0)
                return index_to_price(i);
        }
        return 0.0;
    }

    double get_best_ask() const {
        for (size_t i = 0; i < max_levels_; ++i) {
            if (ask_sizes_[i] > 0.0)
                return index_to_price(i);
        }
        return 0.0;
    }

    double get_mid_price() const {
        double bid = get_best_bid();
        double ask = get_best_ask();
        return (bid > 0.0 && ask > 0.0) ? (bid + ask) / 2.0 : 0.0;
    }

    double get_spread() const { return get_best_ask() - get_best_bid(); }

    uint64_t get_sequence() const { return sequence_.load(std::memory_order_acquire); }

    bool is_initialized() const { return initialized_.load(std::memory_order_acquire); }

    void get_top_levels(size_t n, std::vector<PriceLevel>& bids,
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

    double tick_size() const { return tick_size_; }
    size_t max_levels() const { return max_levels_; }
    double base_price() const { return base_price_.load(std::memory_order_acquire); }
    const std::string& symbol() const { return symbol_; }
    Exchange exchange() const { return exchange_; }

  private:
    static constexpr double k_max_spread_ticks_multiplier_ = 0.25;

    double max_allowed_spread_abs() const noexcept {
        return std::max(tick_size_, static_cast<double>(max_levels_) * tick_size_ *
                                        k_max_spread_ticks_multiplier_);
    }

    static uint32_t fnv1a_bytes(const char* data, size_t len, uint32_t seed) noexcept {
        uint32_t hash = seed;
        for (size_t i = 0; i < len; ++i) {
            hash ^= static_cast<uint8_t>(data[i]);
            hash *= 16777619u;
        }
        return hash;
    }

    static uint32_t compute_snapshot_checksum(const Snapshot& snapshot) noexcept {
        uint32_t hash = 2166136261u;
        auto hash_level = [&hash](const PriceLevel& level) {
            hash =
                fnv1a_bytes(reinterpret_cast<const char*>(&level.price), sizeof(level.price), hash);
            hash =
                fnv1a_bytes(reinterpret_cast<const char*>(&level.size), sizeof(level.size), hash);
        };

        for (const auto& level : snapshot.bids)
            hash_level(level);
        for (const auto& level : snapshot.asks)
            hash_level(level);
        return hash;
    }

    std::string symbol_;
    Exchange exchange_;
    double tick_size_;
    size_t max_levels_;

    std::atomic<double> base_price_;
    std::atomic<uint64_t> sequence_;
    std::atomic<bool> initialized_;

    std::vector<double> bid_sizes_;
    std::vector<double> ask_sizes_;
    std::vector<double> scratch_bid_sizes_;
    std::vector<double> scratch_ask_sizes_;
    uint32_t out_of_range_streak_;

    static constexpr uint32_t k_recenter_streak_trigger_ = 4;
    static constexpr double k_recenter_hard_breach_ratio_ = 0.6;

    bool should_recenter(double price) {
        const double base = base_price_.load(std::memory_order_acquire);
        const double relative = (price - base) / tick_size_;
        const double upper_idx = static_cast<double>(max_levels_ - 1);

        const double breach_ticks =
            (relative < 0.0) ? -relative : (relative > upper_idx ? relative - upper_idx : 0.0);
        if (breach_ticks <= 0.0) {
            out_of_range_streak_ = 0;
            return false;
        }

        out_of_range_streak_++;
        const double hard_breach_ticks =
            static_cast<double>(max_levels_) * k_recenter_hard_breach_ratio_;
        return breach_ticks >= hard_breach_ticks ||
               out_of_range_streak_ >= k_recenter_streak_trigger_;
    }

    void recenter_grid(double anchor_price) {
        const double old_base = base_price_.load(std::memory_order_acquire);
        const double new_base = anchor_price - static_cast<double>(max_levels_ / 2) * tick_size_;
        const int64_t shift_ticks =
            static_cast<int64_t>(std::llround((old_base - new_base) / tick_size_));

        std::fill(scratch_bid_sizes_.begin(), scratch_bid_sizes_.end(), 0.0);
        std::fill(scratch_ask_sizes_.begin(), scratch_ask_sizes_.end(), 0.0);

        for (size_t i = 0; i < max_levels_; ++i) {
            const int64_t new_idx = static_cast<int64_t>(i) + shift_ticks;
            if (new_idx < 0 || static_cast<size_t>(new_idx) >= max_levels_) {
                continue;
            }

            scratch_bid_sizes_[static_cast<size_t>(new_idx)] = bid_sizes_[i];
            scratch_ask_sizes_[static_cast<size_t>(new_idx)] = ask_sizes_[i];
        }

        bid_sizes_.swap(scratch_bid_sizes_);
        ask_sizes_.swap(scratch_ask_sizes_);
        base_price_.store(new_base, std::memory_order_release);
        out_of_range_streak_ = 0;

        LOG_WARN("Order book grid recentered", "symbol", symbol_.c_str(), "anchor_price",
                 anchor_price, "old_base", old_base, "new_base", new_base, "shift_ticks",
                 shift_ticks);
    }

    // Maps price → flat-array index. Returns false if uninitialized or out of range.
    bool price_to_index(double price, size_t& out_idx) const {
        if (!initialized_.load(std::memory_order_acquire))
            return false;
        double base = base_price_.load(std::memory_order_acquire);
        double relative = (price - base) / tick_size_;
        int64_t idx = static_cast<int64_t>(relative + 0.5); // round to nearest tick
        if (idx < 0 || static_cast<size_t>(idx) >= max_levels_)
            return false;
        out_idx = static_cast<size_t>(idx);
        return true;
    }

    double index_to_price(size_t idx) const {
        return base_price_.load(std::memory_order_acquire) + static_cast<double>(idx) * tick_size_;
    }
};

} // namespace trading
