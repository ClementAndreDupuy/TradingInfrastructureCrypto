#pragma once

#include "../../../common/logging.hpp"
#include "../connectors/exchange_connector.hpp"
#include "../portfolio/position_ledger.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>

namespace trading {
    template<typename T, size_t N>
    class SpscQueue {
        static_assert((N & (N - 1)) == 0, "N must be a power of two");
        static_assert(N >= 2, "N must be at least 2");

    public:
        bool push(const T &item) noexcept {
            const size_t w = write_.load(std::memory_order_relaxed);
            const size_t next = (w + 1) & mask_;
            if (next == read_.load(std::memory_order_acquire))
                return false;
            buf_[w] = item;
            write_.store(next, std::memory_order_release);
            return true;
        }

        bool pop(T &item) noexcept {
            const size_t r = read_.load(std::memory_order_relaxed);
            if (r == write_.load(std::memory_order_acquire))
                return false;
            item = buf_[r];
            read_.store((r + 1) & mask_, std::memory_order_release);
            return true;
        }

    private:
        static constexpr size_t mask_ = N - 1;

        alignas(64) std::atomic<size_t> write_{0};
        alignas(64) std::atomic<size_t> read_{0};
        std::array<T, N> buf_{};
    };

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
        static constexpr size_t FILL_QUEUE_DEPTH = 128;

        using FillCallback = std::function<void(const ManagedOrder &, const FillUpdate &)>;

        explicit OrderManager(ExchangeConnector &connector) : connector_(connector), next_id_(1) {
            connector_.on_fill = [this](const FillUpdate &u) {
                if (!fill_queue_.push(u))
                    LOG_ERROR("OrderManager: fill queue full — fill dropped", "client_id",
                          u.client_order_id);
            };
        }

        uint64_t submit(Order order) {
            ManagedOrder *slot = find_free_slot();
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
            ledger_.on_order_submitted(order);
            return order.client_order_id;
        }

        ConnectorResult cancel(uint64_t client_id) { return connector_.cancel_order(client_id); }

        ConnectorResult cancel_all(const char *symbol) { return connector_.cancel_all(symbol); }

        void drain_fills() {
            FillUpdate u;
            while (fill_queue_.pop(u))
                apply_fill(u);
        }

        double position() const noexcept { return position_; }
        double realized_pnl() const noexcept { return ledger_.snapshot().realized_pnl; }

        void update_mid_price(const char *symbol, Exchange exchange, double mid_price) noexcept {
            ledger_.update_mid_price(symbol, exchange, mid_price);
        }

        PositionLedgerSnapshot ledger_snapshot() const noexcept { return ledger_.snapshot(); }

        uint32_t active_order_count() const noexcept {
            uint32_t n = 0;
            for (const auto &s: slots_)
                if (s.active)
                    ++n;
            return n;
        }

        FillCallback on_fill;

    private:
        ExchangeConnector &connector_;
        std::atomic<uint64_t> next_id_;
        std::array<ManagedOrder, MAX_ORDERS> slots_{};
        double position_ = 0.0;
        PositionLedger ledger_;

        SpscQueue<FillUpdate, FILL_QUEUE_DEPTH> fill_queue_;

        void apply_fill(const FillUpdate &u) {
            ManagedOrder *mo = find_order(u.client_order_id);
            if (!mo)
                return;

            mo->filled_qty = u.cumulative_filled_qty;
            mo->avg_fill_price = u.avg_fill_price;
            mo->state = u.new_state;

            if (u.fill_qty > 0.0) {
                double sign = (mo->order.side == Side::BID) ? 1.0 : -1.0;
                position_ += sign * u.fill_qty;
                ledger_.on_fill(mo->order, u);
            }

            if (u.new_state == OrderState::FILLED || u.new_state == OrderState::CANCELED ||
                u.new_state == OrderState::REJECTED) {
                ledger_.on_order_closed(mo->order, std::max(0.0, mo->order.quantity - mo->filled_qty));
                if (on_fill)
                    on_fill(*mo, u);
                mo->active = false;
            }
        }

        ManagedOrder *find_free_slot() {
            for (auto &s: slots_)
                if (!s.active)
                    return &s;
            return nullptr;
        }

        ManagedOrder *find_order(uint64_t client_id) {
            for (auto &s: slots_) {
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
}
