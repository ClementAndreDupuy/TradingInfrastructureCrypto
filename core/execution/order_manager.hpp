#pragma once

// Central order lifecycle tracker.
//
// Strategy registers orders before submission; fill updates arrive from
// ExchangeConnector callbacks and are forwarded to the strategy.
//
// Pre-allocated pool — no heap allocation after construction.
// Thread safety: add_order from strategy thread; on_fill_update from fill thread.

#include "exchange_connector.hpp"
#include <array>
#include <atomic>
#include <functional>

namespace trading {

static constexpr size_t MAX_ORDERS = 1024;

class OrderManager {
public:
    // Called with the final Order state and the triggering FillUpdate.
    using FillNotify = std::function<void(const Order&, const FillUpdate&)>;

    OrderManager() : order_count_(0), next_client_id_(1) {}

    // Allocate a unique client order ID.
    uint64_t next_client_id() noexcept {
        return next_client_id_.fetch_add(1, std::memory_order_relaxed);
    }

    // Register an order slot before submission. Returns pointer into pool or nullptr.
    Order* add_order(const Order& order) noexcept {
        uint32_t idx = order_count_.fetch_add(1, std::memory_order_relaxed);
        if (idx >= MAX_ORDERS) {
            order_count_.fetch_sub(1, std::memory_order_relaxed);
            LOG_ERROR("Order pool exhausted", "max", MAX_ORDERS);
            return nullptr;
        }
        orders_[idx]       = order;
        orders_[idx].state = OrderState::SUBMITTED;
        return &orders_[idx];
    }

    // Process an exchange fill/status update. Called from the fill thread.
    void on_fill_update(const FillUpdate& update) noexcept {
        Order* order = find_by_client_id(update.client_order_id);
        if (!order) {
            LOG_WARN("Unknown client order id in fill", "id", update.client_order_id);
            return;
        }

        // Update VWAP: prefer exchange-reported avg_price, else compute.
        if (update.fill_qty > 0.0) {
            if (update.avg_fill_price > 0.0) {
                order->avg_fill_price = update.avg_fill_price;
            } else if (update.cumulative_filled_qty > 0.0) {
                double prev = order->filled_qty;
                order->avg_fill_price = (prev > 0.0 && order->avg_fill_price > 0.0)
                    ? (order->avg_fill_price * prev + update.fill_price * update.fill_qty)
                      / update.cumulative_filled_qty
                    : update.fill_price;
            }
        }

        order->filled_qty        = update.cumulative_filled_qty;
        order->state             = update.new_state;
        order->last_update_ts_ns = update.local_ts_ns;

        if (order->exchange_order_id[0] == '\0' && update.exchange_order_id[0] != '\0')
            std::strncpy(order->exchange_order_id, update.exchange_order_id, 31);

        LOG_INFO("Order updated",
                 "client_id", update.client_order_id,
                 "state",     order_state_to_string(update.new_state),
                 "fill_qty",  update.fill_qty,
                 "fill_px",   update.fill_price);

        if (fill_notify_) fill_notify_(*order, update);
    }

    void set_fill_notify(FillNotify fn) { fill_notify_ = std::move(fn); }

    Order* find_by_client_id(uint64_t id) noexcept {
        uint32_t count = order_count_.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < count; ++i)
            if (orders_[i].client_order_id == id) return &orders_[i];
        return nullptr;
    }

    // Count of orders in SUBMITTED / PENDING_NEW / ACTIVE state.
    uint32_t active_count() const noexcept {
        uint32_t n = 0;
        uint32_t count = order_count_.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < count; ++i) {
            auto s = orders_[i].state;
            if (s == OrderState::SUBMITTED ||
                s == OrderState::PENDING_NEW ||
                s == OrderState::ACTIVE)
                ++n;
        }
        return n;
    }

    uint32_t total_count() const noexcept {
        return order_count_.load(std::memory_order_acquire);
    }

private:
    std::array<Order, MAX_ORDERS> orders_;
    std::atomic<uint32_t>         order_count_;
    std::atomic<uint64_t>         next_client_id_;
    FillNotify                    fill_notify_;
};

}  // namespace trading
