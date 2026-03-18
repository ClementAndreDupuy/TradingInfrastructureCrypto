#pragma once

// OrderManager — tracks open orders and net position per symbol.
//
// Sits between the strategy and ExchangeConnector:
//   strategy → OrderManager::submit() → connector.submit_order()
//   connector.on_fill → OrderManager::handle_fill() → strategy callback
//
// Pre-allocated slot pool — no heap allocation after construction.
// All operations are O(MAX_ORDERS) worst case; fine for small open-order sets.
//
// ── Threading contract ────────────────────────────────────────────────────────
//
// Shadow / back-test mode (single-threaded):
//   All methods are called from a single thread.  drain_fills() is a no-op
//   because handle_fill() is invoked synchronously by the connector and the
//   SPSC queue is never written to.  Callers may still call drain_fills() each
//   iteration without harm.
//
// Live mode (two threads):
//   • Receive thread  — the WebSocket / FIX thread that drives the connector.
//     It calls the connector's on_fill callback which enqueues a FillUpdate
//     into the SPSC fill queue.  No slot or position state is touched here.
//   • Strategy thread — calls submit(), cancel(), cancel_all(),
//     drain_fills(), active_order_count(), position(), realized_pnl().
//     drain_fills() dequeues every pending FillUpdate and applies mutations
//     (slot state, position_, realized_pnl_) exclusively on this thread,
//     then fires the on_fill strategy callback.
//
//   The strategy thread MUST call drain_fills() at the start of each
//   iteration (before submit()) so that position and slot state reflect all
//   fills received since the last iteration.
//
//   This design eliminates mutex latency on the hot path: the receive thread
//   never blocks, and the strategy thread never contends with it.
// ─────────────────────────────────────────────────────────────────────────────

#include "../common/logging.hpp"
#include "exchange_connector.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>

namespace trading {

// ── Lock-free SPSC ring buffer ────────────────────────────────────────────────
// N must be a power of two.  Producer owns write_; consumer owns read_.
// Capacity is N-1 elements (one slot reserved to distinguish full from empty).
template <typename T, size_t N>
class SpscQueue {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");
    static_assert(N >= 2, "N must be at least 2");

  public:
    // Called from the producer thread only.
    bool push(const T& item) noexcept {
        const size_t w    = write_.load(std::memory_order_relaxed);
        const size_t next = (w + 1) & mask_;
        if (next == read_.load(std::memory_order_acquire))
            return false; // queue full — drop (should not happen under normal load)
        buf_[w] = item;
        write_.store(next, std::memory_order_release);
        return true;
    }

    // Called from the consumer thread only.
    bool pop(T& item) noexcept {
        const size_t r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire))
            return false; // queue empty
        item = buf_[r];
        read_.store((r + 1) & mask_, std::memory_order_release);
        return true;
    }

  private:
    static constexpr size_t mask_ = N - 1;

    alignas(64) std::atomic<size_t> write_{0}; // producer cache line
    alignas(64) std::atomic<size_t> read_{0};  // consumer cache line
    std::array<T, N> buf_{};
};

// ─────────────────────────────────────────────────────────────────────────────

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

    // Fill-queue depth: 128 slots (power of two).  At most 64 orders can be
    // in-flight; 128 provides headroom for rapid partial-fill sequences.
    static constexpr size_t FILL_QUEUE_DEPTH = 128;

    using FillCallback = std::function<void(const ManagedOrder&, const FillUpdate&)>;

    explicit OrderManager(ExchangeConnector& connector) : connector_(connector), next_id_(1) {
        // In live mode the connector invokes this lambda from the receive
        // thread.  We only enqueue — no shared mutable state is touched here.
        connector_.on_fill = [this](const FillUpdate& u) {
            if (!fill_queue_.push(u))
                LOG_ERROR("OrderManager: fill queue full — fill dropped", "client_id",
                          u.client_order_id);
        };
    }

    // Submit an order.  Called from the strategy thread only.
    // Returns the client_order_id on success, 0 on failure.
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

    // Drain all pending fill events from the SPSC queue and apply mutations.
    // Called from the strategy thread at the start of each iteration.
    // Safe to call in single-threaded (shadow) mode — the queue will simply
    // be empty because handle_fill() is invoked synchronously by the connector.
    //
    // Note: in shadow/single-thread mode the connector fires on_fill from the
    // same thread, so fills arrive via the queue exactly like live mode — the
    // code path is identical.
    void drain_fills() {
        FillUpdate u;
        while (fill_queue_.pop(u))
            apply_fill(u);
    }

    // Position query (positive = long, negative = short).
    // Called from the strategy thread only.
    double position() const noexcept { return position_; }
    double realized_pnl() const noexcept { return realized_pnl_; }

    uint32_t active_order_count() const noexcept {
        uint32_t n = 0;
        for (const auto& s : slots_)
            if (s.active)
                ++n;
        return n;
    }

    // Strategy callback fired for each fill/cancel/reject.
    // Always invoked from the strategy thread (inside drain_fills).
    FillCallback on_fill;

  private:
    ExchangeConnector& connector_;
    std::atomic<uint64_t> next_id_;
    std::array<ManagedOrder, MAX_ORDERS> slots_{};
    double position_ = 0.0;
    double realized_pnl_ = 0.0;

    SpscQueue<FillUpdate, FILL_QUEUE_DEPTH> fill_queue_;

    // Apply a single fill event.  Must be called from the strategy thread.
    void apply_fill(const FillUpdate& u) {
        ManagedOrder* mo = find_order(u.client_order_id);
        if (!mo)
            return;

        mo->filled_qty = u.cumulative_filled_qty;
        mo->avg_fill_price = u.avg_fill_price;
        mo->state = u.new_state;

        if (u.new_state == OrderState::FILLED || u.new_state == OrderState::CANCELED ||
            u.new_state == OrderState::REJECTED) {

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
