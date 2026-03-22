#include "core/execution/common/order_manager.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace trading {
namespace {

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
    EXPECT_EQ(callback_count, 0);
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

    EXPECT_EQ(om.active_order_count(), 8u);

    om.drain_fills();

    EXPECT_DOUBLE_EQ(om.position(), 8.0);
    EXPECT_EQ(om.active_order_count(), 0u);
}

TEST(OrderManagerTest, PoolFullRejectsSubmit) {
    MockConnector conn;
    OrderManager om(conn);

    for (size_t i = 0; i < OrderManager::MAX_ORDERS; ++i)
        EXPECT_GT(om.submit(make_order()), 0u);

    EXPECT_EQ(om.submit(make_order()), 0u);
}

TEST(OrderManagerTest, UnknownFillIdIsIgnored) {
    MockConnector conn;
    OrderManager om(conn);

    int callback_count = 0;
    om.on_fill = [&](const ManagedOrder&, const FillUpdate&) { ++callback_count; };

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

    om.drain_fills();
    EXPECT_DOUBLE_EQ(om.position(), 0.0);
}

TEST(OrderManagerTest, RealizedPnlAccumulatesCorrectly) {
    MockConnector conn;
    OrderManager om(conn);

    uint64_t buy_id = om.submit(make_order(Side::BID, 50000.0, 2.0));
    conn.emit(make_fill(buy_id, 2.0, 50000.0));
    om.drain_fills();
    EXPECT_DOUBLE_EQ(om.realized_pnl(), -100000.0);

    uint64_t sell_id = om.submit(make_order(Side::ASK, 51000.0, 2.0));
    conn.emit(make_fill(sell_id, 2.0, 51000.0));
    om.drain_fills();
    EXPECT_DOUBLE_EQ(om.realized_pnl(), 2000.0);
}

TEST(OrderManagerTest, LedgerTracksPartialUnwindAndPendingOrders) {
    MockConnector conn;
    OrderManager om(conn);

    const uint64_t buy_id = om.submit(make_order(Side::BID, 50000.0, 3.0));
    ASSERT_GT(buy_id, 0u);
    conn.emit(make_fill(buy_id, 3.0, 50000.0));
    om.drain_fills();

    const uint64_t sell_id = om.submit(make_order(Side::ASK, 51000.0, 1.0));
    ASSERT_GT(sell_id, 0u);
    conn.emit(make_fill(sell_id, 1.0, 51000.0));
    om.drain_fills();

    const auto snapshot = om.ledger_snapshot();
    ASSERT_EQ(snapshot.venue_count, 1u);
    EXPECT_DOUBLE_EQ(snapshot.global_position, 2.0);
    EXPECT_DOUBLE_EQ(snapshot.global_avg_entry_price, 50000.0);
    EXPECT_DOUBLE_EQ(snapshot.realized_pnl, 1000.0);
    EXPECT_DOUBLE_EQ(snapshot.pending_bid_qty, 0.0);
    EXPECT_DOUBLE_EQ(snapshot.pending_ask_qty, 0.0);
    EXPECT_DOUBLE_EQ(snapshot.venues[0].position, 2.0);
    EXPECT_DOUBLE_EQ(snapshot.venues[0].avg_entry_price, 50000.0);
}

TEST(OrderManagerTest, LedgerTracksPartialFillsBeforeFinalClose) {
    MockConnector conn;
    OrderManager om(conn);

    const uint64_t id = om.submit(make_order(Side::BID, 50000.0, 3.0));
    ASSERT_GT(id, 0u);

    conn.emit(make_fill(id, 1.0, 50000.0, OrderState::PARTIALLY_FILLED));
    om.drain_fills();

    auto snapshot = om.ledger_snapshot();
    ASSERT_EQ(snapshot.venue_count, 1u);
    EXPECT_DOUBLE_EQ(snapshot.global_position, 1.0);
    EXPECT_DOUBLE_EQ(snapshot.pending_bid_qty, 2.0);

    FillUpdate final_fill = make_fill(id, 2.0, 50010.0);
    final_fill.cumulative_filled_qty = 3.0;
    final_fill.avg_fill_price = (50000.0 + 2.0 * 50010.0) / 3.0;
    conn.emit(final_fill);
    om.drain_fills();

    snapshot = om.ledger_snapshot();
    EXPECT_DOUBLE_EQ(snapshot.global_position, 3.0);
    EXPECT_DOUBLE_EQ(snapshot.pending_bid_qty, 0.0);
    EXPECT_NEAR(snapshot.global_avg_entry_price, final_fill.avg_fill_price, 1e-9);
}

TEST(OrderManagerTest, LedgerTracksCrossVenueInventoryAndMidMarks) {
    MockConnector conn;
    OrderManager om(conn);

    auto okx_buy = make_order(Side::BID, 100.0, 1.5);
    okx_buy.exchange = Exchange::OKX;
    const uint64_t okx_id = om.submit(okx_buy);
    ASSERT_GT(okx_id, 0u);
    conn.emit(make_fill(okx_id, 1.5, 100.0));
    om.drain_fills();

    auto kraken_buy = make_order(Side::BID, 101.0, 0.5);
    kraken_buy.exchange = Exchange::KRAKEN;
    const uint64_t kraken_id = om.submit(kraken_buy);
    ASSERT_GT(kraken_id, 0u);
    conn.emit(make_fill(kraken_id, 0.5, 101.0));
    om.drain_fills();

    om.update_mid_price("BTCUSDT", Exchange::OKX, 102.0);
    om.update_mid_price("BTCUSDT", Exchange::KRAKEN, 99.0);

    const auto snapshot = om.ledger_snapshot();
    ASSERT_EQ(snapshot.venue_count, 2u);
    EXPECT_DOUBLE_EQ(snapshot.global_position, 2.0);
    EXPECT_DOUBLE_EQ(snapshot.global_avg_entry_price, 100.25);
    EXPECT_DOUBLE_EQ(snapshot.unrealized_pnl, 2.5);
}

TEST(OrderManagerTest, LedgerTracksInventoryAgeAfterEntry) {
    MockConnector conn;
    OrderManager om(conn);

    const uint64_t buy_id = om.submit(make_order(Side::BID, 50000.0, 1.0));
    ASSERT_GT(buy_id, 0u);
    conn.emit(make_fill(buy_id, 1.0, 50000.0));
    om.drain_fills();

    std::this_thread::sleep_for(std::chrono::milliseconds(2));

    const auto snapshot = om.ledger_snapshot();
    ASSERT_EQ(snapshot.venue_count, 1u);
    EXPECT_GE(snapshot.oldest_inventory_age_ms, 1);
    EXPECT_GE(snapshot.venues[0].inventory_age_ms, 1);
}

TEST(OrderManagerTest, LedgerResetsAveragePriceOnFullFlatten) {
    MockConnector conn;
    OrderManager om(conn);

    const uint64_t buy_id = om.submit(make_order(Side::BID, 50000.0, 1.0));
    ASSERT_GT(buy_id, 0u);
    conn.emit(make_fill(buy_id, 1.0, 50000.0));
    om.drain_fills();

    const uint64_t sell_id = om.submit(make_order(Side::ASK, 50500.0, 1.0));
    ASSERT_GT(sell_id, 0u);
    conn.emit(make_fill(sell_id, 1.0, 50500.0));
    om.drain_fills();

    const auto snapshot = om.ledger_snapshot();
    EXPECT_DOUBLE_EQ(snapshot.global_position, 0.0);
    EXPECT_DOUBLE_EQ(snapshot.global_avg_entry_price, 0.0);
    EXPECT_DOUBLE_EQ(snapshot.realized_pnl, 500.0);
    EXPECT_EQ(snapshot.oldest_inventory_age_ms, 0);
}

TEST(OrderManagerTest, ConcurrentFillsAndStrategyOps_NoDataRace) {
    MockConnector conn;
    OrderManager om(conn);

    constexpr int ROUNDS = 2000;

    std::atomic<bool> done{false};
    std::atomic<uint64_t> pending_id{0};
    std::atomic<bool> fill_ready{false};

    std::thread strategy_thread([&] {
        for (int i = 0; i < ROUNDS; ++i) {
            uint64_t id = om.submit(make_order(Side::BID, 50000.0, 1.0));
            pending_id.store(id, std::memory_order_relaxed);
            fill_ready.store(true, std::memory_order_release);

            while (fill_ready.load(std::memory_order_acquire))
                std::this_thread::yield();

            om.drain_fills();
            (void)om.active_order_count();
            (void)om.position();
        }
        done.store(true, std::memory_order_release);
    });

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

    om.drain_fills();
    EXPECT_DOUBLE_EQ(om.position(), static_cast<double>(ROUNDS));
}

TEST(OrderManagerTest, ConcurrentHighThroughput_NoDataRace) {
    MockConnector conn;
    OrderManager om(conn);

    constexpr int BATCH = 32;
    std::vector<uint64_t> ids;
    ids.reserve(BATCH);
    for (int i = 0; i < BATCH; ++i)
        ids.push_back(om.submit(make_order(Side::BID, 50000.0, 1.0)));

    std::atomic<int> fills_emitted{0};
    std::atomic<bool> start{false};

    std::thread receive_thread([&] {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (uint64_t id : ids) {
            conn.emit(make_fill(id, 1.0, 50000.0));
            fills_emitted.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread strategy_thread([&] {
        start.store(true, std::memory_order_release);
        while (fills_emitted.load(std::memory_order_relaxed) < BATCH) {
            om.drain_fills();
            (void)om.active_order_count();
            (void)om.position();
            std::this_thread::yield();
        }
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
