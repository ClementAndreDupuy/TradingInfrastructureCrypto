#include "core/feeds/common/book_manager.hpp"
#include "core/shadow/shadow_engine.hpp"
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

using namespace trading;

namespace {

Snapshot make_snapshot(Exchange ex, double best_bid, double bid_size, double best_ask,
                       double ask_size) {
    Snapshot s;
    s.symbol = "BTCUSDT";
    s.exchange = ex;
    s.sequence = 1;
    s.bids.emplace_back(best_bid, bid_size);
    s.asks.emplace_back(best_ask, ask_size);
    return s;
}

Order make_limit(Exchange ex, Side side, uint64_t oid, double px, double qty) {
    Order o;
    std::strncpy(o.symbol, "BTCUSDT", sizeof(o.symbol) - 1);
    o.exchange = ex;
    o.side = side;
    o.type = OrderType::LIMIT;
    o.tif = TimeInForce::GTC;
    o.client_order_id = oid;
    o.price = px;
    o.quantity = qty;
    return o;
}

} // namespace

TEST(ShadowEngineTest, RestingOrderProducesPartialThenFinalFill) {
    BookManager book("BTCUSDT", Exchange::BINANCE, 0.5, 2048);
    book.snapshot_handler()(make_snapshot(Exchange::BINANCE, 100.0, 2.0, 100.5, 2.0));

    ShadowConfig cfg;
    cfg.base_latency_ns = 0;
    cfg.latency_jitter_ns = 0;
    cfg.queue_match_fraction_per_check = 0.30;

    ShadowConnector connector(Exchange::BINANCE, cfg, book);
    std::vector<FillUpdate> fills;
    connector.on_fill = [&](const FillUpdate& u) { fills.push_back(u); };

    // Submit a resting ask at the current best ask; queue-ahead at this level
    // should force partial fills once the opposite side moves to cross.
    ASSERT_EQ(connector.submit_order(make_limit(Exchange::BINANCE, Side::ASK, 1, 100.5, 1.0)),
              ConnectorResult::OK);

    Delta make_cross;
    make_cross.side = Side::BID;
    make_cross.price = 100.5;
    make_cross.size = 2.0;
    make_cross.sequence = 2;
    book.delta_handler()(make_cross);

    for (int i = 0; i < 4 && fills.empty(); ++i)
        connector.check_fills();
    ASSERT_FALSE(fills.empty());
    EXPECT_EQ(fills.front().new_state, OrderState::PARTIALLY_FILLED);
    EXPECT_LT(fills.front().fill_qty, 1.0);

    for (int i = 0; i < 12 && connector.active_orders() > 0; ++i)
        connector.check_fills();

    ASSERT_FALSE(fills.empty());
    EXPECT_EQ(fills.back().new_state, OrderState::FILLED);
    EXPECT_NEAR(fills.back().cumulative_filled_qty, 1.0, 1e-9);
}

TEST(ShadowEngineTest, FeeModelUsesCoinbaseAndOkxSpecificRates) {
    BookManager book("BTCUSDT", Exchange::OKX, 1.0, 2048);
    book.snapshot_handler()(make_snapshot(Exchange::OKX, 100.0, 10.0, 100.5, 10.0));

    ShadowConfig cfg;
    cfg.base_latency_ns = 0;
    cfg.latency_jitter_ns = 0;
    cfg.impact_slippage_per_notional_bps = 0.0;
    cfg.okx_taker_fee_bps = 20.0;
    cfg.coinbase_taker_fee_bps = 70.0;

    ShadowConnector okx(Exchange::OKX, cfg, book);
    ShadowConnector coinbase(Exchange::COINBASE, cfg, book);

    ASSERT_EQ(okx.submit_order(make_limit(Exchange::OKX, Side::ASK, 10, 99.0, 1.0)),
              ConnectorResult::OK);
    ASSERT_EQ(coinbase.submit_order(make_limit(Exchange::COINBASE, Side::ASK, 11, 99.0, 1.0)),
              ConnectorResult::OK);

    const double okx_pnl = okx.total_pnl();
    const double coinbase_pnl = coinbase.total_pnl();

    EXPECT_GT(okx_pnl, coinbase_pnl);
    EXPECT_NEAR(okx_pnl - coinbase_pnl, 0.5, 1e-3);
}

TEST(ShadowEngineTest, LatencyGateDelaysFillUntilReleaseTime) {
    BookManager book("BTCUSDT", Exchange::BINANCE, 1.0, 2048);
    book.snapshot_handler()(make_snapshot(Exchange::BINANCE, 100.0, 2.0, 100.5, 2.0));

    ShadowConfig cfg;
    cfg.base_latency_ns = 1000000000;
    cfg.latency_jitter_ns = 0;

    ShadowConnector connector(Exchange::BINANCE, cfg, book);
    std::vector<FillUpdate> fills;
    connector.on_fill = [&](const FillUpdate& u) { fills.push_back(u); };

    ASSERT_EQ(connector.submit_order(make_limit(Exchange::BINANCE, Side::BID, 20, 101.0, 0.5)),
              ConnectorResult::OK);
    connector.check_fills();

    EXPECT_TRUE(fills.empty());
    EXPECT_EQ(connector.active_orders(), 1u);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
