#pragma once

// OrderManager — tracks open orders and net position per symbol.
//
// Sits between the strategy and ExchangeConnector:
//   strategy → OrderManager::submit() → connector.submit_order()
//   connector.on_fill → OrderManager::handle_fill() → strategy callback
//
// Pre-allocated slot pool — no heap allocation after construction.
// All operations are O(MAX_ORDERS) worst case; fine for small open-order sets.

#include "../common/logging.hpp"
#include "exchange_connector.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>

namespace trading {

struct ManagedOrder {
    Order order;
    OrderState state = OrderState::PENDING;
    double filled_qty = 0.0;
    double avg_fill_price = 0.0;
    bool active = false;
};

class OrderManager {
  public:
    static constexpr size_t MAX_ORDERS = 64;

    using FillCallback = std::function<void(const ManagedOrder&, const FillUpdate&)>;

    explicit OrderManager(ExchangeConnector& connector) : connector_(connector), next_id_(1) {
        connector_.on_fill = [this](const FillUpdate& u) { handle_fill(u); };
    }

    // Submit an order. Returns the client_order_id on success, 0 on failure.
    uint64_t submit(Order order) {
        ManagedOrder* slot = find_free_slot();
        if (!slot) {
            LOG_ERROR("OrderManager: pool full — order rejected", "component", "order_manager");
            return 0;
        }

        order.client_order_id = next_id_.fetch_add(1, std::memory_order_relaxed);
        order.submit_ts_ns = now_ns();

        slot->order = order;
        slot->state = OrderState::PENDING;
        slot->filled_qty = 0.0;
        slot->avg_fill_price = 0.0;
        slot->active = true;

        auto result = connector_.submit_order(order);
        if (result != ConnectorResult::OK) {
            slot->active = false;
            LOG_ERROR("OrderManager: submit_order rejected", "client_id", order.client_order_id);
            return 0;
        }
        return order.client_order_id;
    }

    ConnectorResult cancel(uint64_t client_id) { return connector_.cancel_order(client_id); }

    ConnectorResult cancel_all(const char* symbol) { return connector_.cancel_all(symbol); }

    // Process fill/cancel/reject from connector.
    void handle_fill(const FillUpdate& u) {
        ManagedOrder* mo = find_order(u.client_order_id);
        if (!mo)
            return;

        mo->filled_qty = u.cumulative_filled_qty;
        mo->avg_fill_price = u.avg_fill_price;
        mo->state = u.new_state;

        if (u.new_state == OrderState::FILLED || u.new_state == OrderState::CANCELED ||
            u.new_state == OrderState::REJECTED) {

            // Update net position on fills
            if (u.new_state == OrderState::FILLED && u.fill_qty > 0.0) {
                double sign = (mo->order.side == Side::BID) ? 1.0 : -1.0;
                position_ += sign * u.fill_qty;
                realized_pnl_ -= sign * u.fill_price * u.fill_qty; // cost basis
            }

            if (on_fill)
                on_fill(*mo, u);
            mo->active = false;
        }
    }

    // Position query (positive = long, negative = short)
    double position() const noexcept { return position_; }
    double realized_pnl() const noexcept { return realized_pnl_; }

    uint32_t active_order_count() const noexcept {
        uint32_t n = 0;
        for (const auto& s : slots_)
            if (s.active)
                ++n;
        return n;
    }

    // Strategy callback for fills
    FillCallback on_fill;

  private:
    ExchangeConnector& connector_;
    std::atomic<uint64_t> next_id_;
    std::array<ManagedOrder, MAX_ORDERS> slots_{};
    double position_ = 0.0;
    double realized_pnl_ = 0.0;

    ManagedOrder* find_free_slot() {
        for (auto& s : slots_)
            if (!s.active)
                return &s;
        return nullptr;
    }

    ManagedOrder* find_order(uint64_t client_id) {
        for (auto& s : slots_) {
            if (s.active && s.order.client_order_id == client_id)
                return &s;
        }
        return nullptr;
    }

    static int64_t now_ns() noexcept {
        using namespace std::chrono;
        return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
    }
};

} // namespace trading
