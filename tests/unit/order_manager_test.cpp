// order_manager_test.cpp — unit + concurrency tests for OrderManager.
//
// Tests cover:
//   1. Basic single-thread semantics (submit, fill, position accounting).
//   2. Fill-queue mechanics (fills land in queue; drain_fills processes them).
//   3. Pool-full rejection.
//   4. Concurrent on_fill (receive thread) + submit/drain_fills/active_order_count
//      (strategy thread) — safe under ThreadSanitizer (TSAN).

#include "core/execution/order_manager.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace trading {
namespace {

// ── Helpers ───────────────────────────────────────────────────────────────────

class MockConnector final : public ExchangeConnector {
  public:
    Exchange exchange_id() const override { return Exchange::BINANCE; }
    bool is_connected() const override { return true; }
    ConnectorResult connect() override { return ConnectorResult::OK; }
    void disconnect() override {}

    ConnectorResult submit_order(const Order& order) override {
        last_submitted = order;
        ++submit_count;
        return submit_result;
    }

    ConnectorResult cancel_order(uint64_t) override { return ConnectorResult::OK; }
    ConnectorResult replace_order(uint64_t, const Order&) override { return ConnectorResult::OK; }
    ConnectorResult query_order(uint64_t, FillUpdate&) override { return ConnectorResult::OK; }
    ConnectorResult cancel_all(const char*) override { return ConnectorResult::OK; }
    ConnectorResult reconcile() override { return ConnectorResult::OK; }

    // Trigger on_fill from the caller's thread (simulates receive thread).
    void emit(const FillUpdate& u) {
        if (on_fill)
            on_fill(u);
    }

    ConnectorResult submit_result = ConnectorResult::OK;
    Order last_submitted{};
    int submit_count = 0;
};

static Order make_order(Side side = Side::BID, double price = 50000.0,
                        double qty = 1.0) noexcept {
    Order o;
    std::strncpy(o.symbol, "BTCUSDT", sizeof(o.symbol) - 1);
    o.symbol[sizeof(o.symbol) - 1] = '\0';
    o.exchange = Exchange::BINANCE;
    o.side = side;
    o.type = OrderType::LIMIT;
    o.tif = TimeInForce::GTC;
    o.price = price;
    o.quantity = qty;
    return o;
}

static FillUpdate make_fill(uint64_t id, double qty, double price,
                            OrderState state = OrderState::FILLED) noexcept {
    FillUpdate u;
    u.client_order_id = id;
    u.fill_qty = qty;
    u.fill_price = price;
    u.cumulative_filled_qty = qty;
    u.avg_fill_price = price;
    u.new_state = state;
    return u;
}

// ── Single-thread: basic submit/fill/position ─────────────────────────────────

TEST(OrderManagerTest, SubmitSucceeds) {
    MockConnector conn;
    OrderManager om(conn);

    uint64_t id = om.submit(make_order());
    EXPECT_GT(id, 0u);
    EXPECT_EQ(conn.submit_count, 1);
    EXPECT_EQ(om.active_order_count(), 1u);
}

TEST(OrderManagerTest, FillNotVisibleBeforeDrain) {
    MockConnector conn;
    OrderManager om(conn);

    uint64_t id = om.submit(make_order(Side::BID, 50000.0, 2.0));
    ASSERT_GT(id, 0u);

    conn.emit(make_fill(id, 2.0, 50000.0));

    // Fill enqueued but not yet applied — slot still looks active, position 0.
    EXPECT_EQ(om.active_order_count(), 1u);
    EXPECT_DOUBLE_EQ(om.position(), 0.0);
}

TEST(OrderManagerTest, DrainAppliesFillAndUpdatesPosition) {
    MockConnector conn;
    OrderManager om(conn);

    uint64_t id = om.submit(make_order(Side::BID, 50000.0, 2.0));
    ASSERT_GT(id, 0u);

    conn.emit(make_fill(id, 2.0, 50000.0));
    om.drain_fills();

    EXPECT_DOUBLE_EQ(om.position(), 2.0);
    EXPECT_EQ(om.active_order_count(), 0u);
}

TEST(OrderManagerTest, PositionTracking_BuySell) {
    MockConnector conn;
    OrderManager om(conn);

    uint64_t buy_id = om.submit(make_order(Side::BID, 50000.0, 3.0));
    conn.emit(make_fill(buy_id, 3.0, 50000.0));
    om.drain_fills();
    EXPECT_DOUBLE_EQ(om.position(), 3.0);

    uint64_t sell_id = om.submit(make_order(Side::ASK, 51000.0, 1.0));
    conn.emit(make_fill(sell_id, 1.0, 51000.0));
    om.drain_fills();
    EXPECT_DOUBLE_EQ(om.position(), 2.0);
}

TEST(OrderManagerTest, CancelAndRejectDoNotMovePosition) {
    MockConnector conn;
    OrderManager om(conn);

    uint64_t id = om.submit(make_order());
    ASSERT_GT(id, 0u);

    conn.emit(make_fill(id, 0.0, 0.0, OrderState::CANCELED));
    om.drain_fills();

    EXPECT_DOUBLE_EQ(om.position(), 0.0);
    EXPECT_EQ(om.active_order_count(), 0u);
}

TEST(OrderManagerTest, FillCallbackFiredFromDrain) {
    MockConnector conn;
    OrderManager om(conn);

    int callback_count = 0;
    om.on_fill = [&](const ManagedOrder&, const FillUpdate&) { ++callback_count; };

    uint64_t id = om.submit(make_order());
    ASSERT_GT(id, 0u);

    conn.emit(make_fill(id, 1.0, 50000.0));
    EXPECT_EQ(callback_count, 0); // not yet
    om.drain_fills();
    EXPECT_EQ(callback_count, 1);
}

TEST(OrderManagerTest, MultipleFillsQueuedAndDrained) {
    MockConnector conn;
    OrderManager om(conn);

    std::vector<uint64_t> ids;
    for (int i = 0; i < 8; ++i)
        ids.push_back(om.submit(make_order()));

    for (uint64_t id : ids)
        conn.emit(make_fill(id, 1.0, 50000.0));

    // All enqueued but none processed yet.
    EXPECT_EQ(om.active_order_count(), 8u);

    om.drain_fills();

    EXPECT_DOUBLE_EQ(om.position(), 8.0);
    EXPECT_EQ(om.active_order_count(), 0u);
}

TEST(OrderManagerTest, PoolFullRejectsSubmit) {
    MockConnector conn;
    OrderManager om(conn);

    // Fill all 64 slots.
    for (size_t i = 0; i < OrderManager::MAX_ORDERS; ++i)
        EXPECT_GT(om.submit(make_order()), 0u);

    // 65th should fail.
    EXPECT_EQ(om.submit(make_order()), 0u);
}

TEST(OrderManagerTest, UnknownFillIdIsIgnored) {
    MockConnector conn;
    OrderManager om(conn);

    int callback_count = 0;
    om.on_fill = [&](const ManagedOrder&, const FillUpdate&) { ++callback_count; };

    // Emit a fill for an ID that was never submitted.
    conn.emit(make_fill(9999u, 1.0, 50000.0));
    om.drain_fills();

    EXPECT_EQ(callback_count, 0);
    EXPECT_DOUBLE_EQ(om.position(), 0.0);
}

TEST(OrderManagerTest, SubmitFailureLeavesSlotFree) {
    MockConnector conn;
    conn.submit_result = ConnectorResult::ERROR_RATE_LIMIT;
    OrderManager om(conn);

    uint64_t id = om.submit(make_order());
    EXPECT_EQ(id, 0u);
    EXPECT_EQ(om.active_order_count(), 0u);
}

TEST(OrderManagerTest, DrainOnEmptyQueueIsNoop) {
    MockConnector conn;
    OrderManager om(conn);

    om.drain_fills(); // should not crash or alter state
    EXPECT_DOUBLE_EQ(om.position(), 0.0);
}

TEST(OrderManagerTest, RealizedPnlAccumulatesCorrectly) {
    MockConnector conn;
    OrderManager om(conn);

    // Buy 2 BTC at 50 000; cost basis = -100 000
    uint64_t buy_id = om.submit(make_order(Side::BID, 50000.0, 2.0));
    conn.emit(make_fill(buy_id, 2.0, 50000.0));
    om.drain_fills();
    // realized_pnl_ = -(+1 * 50000 * 2) = -100000
    EXPECT_DOUBLE_EQ(om.realized_pnl(), -100000.0);

    // Sell 2 BTC at 51 000; reverses cost
    uint64_t sell_id = om.submit(make_order(Side::ASK, 51000.0, 2.0));
    conn.emit(make_fill(sell_id, 2.0, 51000.0));
    om.drain_fills();
    // realized_pnl_ = -100000 - (-1 * 51000 * 2) = -100000 + 102000 = 2000
    EXPECT_DOUBLE_EQ(om.realized_pnl(), 2000.0);
}

// ── Concurrency: receive thread + strategy thread ─────────────────────────────
//
// The receive thread calls connector.emit() (→ enqueues into SPSC) while the
// strategy thread concurrently calls submit(), drain_fills(), and
// active_order_count().  Under TSAN this test must report zero data races.

TEST(OrderManagerTest, ConcurrentFillsAndStrategyOps_NoDataRace) {
    MockConnector conn;
    OrderManager om(conn);

    constexpr int ROUNDS = 2000;

    // Pre-submit one order per round so there are valid IDs to fill.
    // We'll recycle a single slot: submit → fill → drain (slot freed) → repeat.
    // To avoid slot exhaustion the strategy thread submits one per round
    // *before* signalling the receive thread.

    std::atomic<bool> done{false};
    std::atomic<uint64_t> pending_id{0};   // strategy writes, receive thread reads
    std::atomic<bool> fill_ready{false};   // receive thread waits for a valid ID

    // Strategy thread: submit, signal receive thread, drain, check.
    std::thread strategy_thread([&] {
        for (int i = 0; i < ROUNDS; ++i) {
            uint64_t id = om.submit(make_order(Side::BID, 50000.0, 1.0));
            // Store before flagging — fill_ready acts as release fence.
            pending_id.store(id, std::memory_order_relaxed);
            fill_ready.store(true, std::memory_order_release);

            // Spin until receive thread has consumed the fill.
            while (fill_ready.load(std::memory_order_acquire))
                std::this_thread::yield();

            om.drain_fills();

            // These reads of shared state must be race-free.
            (void)om.active_order_count();
            (void)om.position();
        }
        done.store(true, std::memory_order_release);
    });

    // Receive thread: wait for each ID, emit fill, clear signal.
    std::thread receive_thread([&] {
        while (!done.load(std::memory_order_acquire)) {
            if (!fill_ready.load(std::memory_order_acquire)) {
                std::this_thread::yield();
                continue;
            }
            uint64_t id = pending_id.load(std::memory_order_relaxed);
            if (id != 0)
                conn.emit(make_fill(id, 1.0, 50000.0));
            fill_ready.store(false, std::memory_order_release);
        }
    });

    strategy_thread.join();
    receive_thread.join();

    // All fills have been drained; each fill was a 1 BTC buy then no sells,
    // so position should be ROUNDS BTC.
    om.drain_fills(); // catch any final stragglers
    EXPECT_DOUBLE_EQ(om.position(), static_cast<double>(ROUNDS));
}

// Stress variant: receive thread emits fills for already-submitted orders
// while the strategy thread continuously drains and checks active count.
// Validates that no undefined behaviour or data race occurs under higher
// concurrency.  Uses TSAN annotations indirectly through sanitizer build.
TEST(OrderManagerTest, ConcurrentHighThroughput_NoDataRace) {
    MockConnector conn;
    OrderManager om(conn);

    // Pre-submit a batch of orders; collect their IDs.
    constexpr int BATCH = 32;
    std::vector<uint64_t> ids;
    ids.reserve(BATCH);
    for (int i = 0; i < BATCH; ++i)
        ids.push_back(om.submit(make_order(Side::BID, 50000.0, 1.0)));

    std::atomic<int> fills_emitted{0};
    std::atomic<bool> start{false};

    // Receive thread: emit fills for each pre-submitted order.
    std::thread receive_thread([&] {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (uint64_t id : ids) {
            conn.emit(make_fill(id, 1.0, 50000.0));
            fills_emitted.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Strategy thread: drain and sample state while fills arrive.
    std::thread strategy_thread([&] {
        start.store(true, std::memory_order_release);
        while (fills_emitted.load(std::memory_order_relaxed) < BATCH) {
            om.drain_fills();
            (void)om.active_order_count();
            (void)om.position();
            std::this_thread::yield();
        }
        // Final drain to pick up any fills not yet processed.
        om.drain_fills();
    });

    receive_thread.join();
    strategy_thread.join();

    om.drain_fills();
    EXPECT_DOUBLE_EQ(om.position(), static_cast<double>(BATCH));
    EXPECT_EQ(om.active_order_count(), 0u);
}

} // namespace
} // namespace trading
