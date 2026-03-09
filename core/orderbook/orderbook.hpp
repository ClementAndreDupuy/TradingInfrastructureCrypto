#pragma once

#include "../common/types.hpp"
#include "../common/logging.hpp"
#include <array>
#include <atomic>
#include <cstring>

namespace trading {

class OrderBook {
public:
    static constexpr size_t MAX_LEVELS = 10000;
    static constexpr double TICK_SIZE = 0.01;

    OrderBook(const std::string& symbol, Exchange exchange)
        : symbol_(symbol), exchange_(exchange), sequence_(0) {
        clear();
    }

    Result apply_snapshot(const Snapshot& snapshot) {
        if (snapshot.bids.empty() || snapshot.asks.empty()) {
            return Result::ERROR_INVALID_PRICE;
        }

        clear();
        sequence_.store(snapshot.sequence, std::memory_order_release);

        for (const auto& level : snapshot.bids) {
            set_level(Side::BID, level.price, level.size);
        }

        for (const auto& level : snapshot.asks) {
            set_level(Side::ASK, level.price, level.size);
        }

        LOG_INFO("Snapshot applied", "symbol", symbol_.c_str(),
                 "sequence", snapshot.sequence,
                 "bids", snapshot.bids.size(),
                 "asks", snapshot.asks.size());
        return Result::SUCCESS;
    }

    Result apply_delta(const Delta& delta) {
        if (delta.sequence <= sequence_.load(std::memory_order_acquire)) {
            return Result::SUCCESS;
        }

        set_level(delta.side, delta.price, delta.size);
        sequence_.store(delta.sequence, std::memory_order_release);

        return Result::SUCCESS;
    }

    double get_best_bid() const {
        for (size_t i = MAX_LEVELS - 1; i > 0; --i) {
            if (bid_sizes_[i] > 0) {
                return index_to_price(i);
            }
        }
        return 0.0;
    }

    double get_best_ask() const {
        for (size_t i = 0; i < MAX_LEVELS; ++i) {
            if (ask_sizes_[i] > 0) {
                return index_to_price(i);
            }
        }
        return 0.0;
    }

    double get_mid_price() const {
        double bid = get_best_bid();
        double ask = get_best_ask();
        if (bid > 0 && ask > 0) {
            return (bid + ask) / 2.0;
        }
        return 0.0;
    }

    double get_spread() const {
        return get_best_ask() - get_best_bid();
    }

    uint64_t get_sequence() const {
        return sequence_.load(std::memory_order_acquire);
    }

    void get_top_levels(size_t n, std::vector<PriceLevel>& bids, std::vector<PriceLevel>& asks) const {
        bids.clear();
        asks.clear();

        for (size_t i = MAX_LEVELS - 1, count = 0; i > 0 && count < n; --i) {
            if (bid_sizes_[i] > 0) {
                bids.push_back(PriceLevel(index_to_price(i), bid_sizes_[i]));
                count++;
            }
        }

        for (size_t i = 0, count = 0; i < MAX_LEVELS && count < n; ++i) {
            if (ask_sizes_[i] > 0) {
                asks.push_back(PriceLevel(index_to_price(i), ask_sizes_[i]));
                count++;
            }
        }
    }

private:
    std::string symbol_;
    Exchange exchange_;
    std::atomic<uint64_t> sequence_;

    std::array<double, MAX_LEVELS> bid_sizes_;
    std::array<double, MAX_LEVELS> ask_sizes_;

    void clear() {
        bid_sizes_.fill(0.0);
        ask_sizes_.fill(0.0);
    }

    size_t price_to_index(double price) const {
        return static_cast<size_t>(price / TICK_SIZE) % MAX_LEVELS;
    }

    double index_to_price(size_t index) const {
        return index * TICK_SIZE;
    }

    void set_level(Side side, double price, double size) {
        size_t idx = price_to_index(price);
        if (side == Side::BID) {
            bid_sizes_[idx] = size;
        } else {
            ask_sizes_[idx] = size;
        }
    }
};

}  // namespace trading
