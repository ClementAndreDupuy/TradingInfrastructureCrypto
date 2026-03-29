#include "core/execution/common/portfolio/live_session_accounting.hpp"

#include <gtest/gtest.h>

#include <cstring>

namespace {
using namespace trading;

ReconciliationSnapshot make_spot_snapshot() {
    ReconciliationSnapshot snapshot;

    ReconciledBalance usdt;
    std::strncpy(usdt.asset, "USDT", sizeof(usdt.asset) - 1);
    usdt.total = 10100.0;
    usdt.available = 9800.0;
    snapshot.balances.push(usdt);

    ReconciledBalance btc;
    std::strncpy(btc.asset, "BTC", sizeof(btc.asset) - 1);
    btc.total = 1.0;
    btc.available = 1.0;
    snapshot.balances.push(btc);

    ReconciledFill fill;
    std::strncpy(fill.venue_trade_id, "t1", sizeof(fill.venue_trade_id) - 1);
    std::strncpy(fill.symbol, "BTCUSDT", sizeof(fill.symbol) - 1);
    fill.exchange = Exchange::BINANCE;
    fill.side = Side::BID;
    fill.quantity = 0.1;
    fill.price = 50000.0;
    fill.fee = 2.0;
    std::strncpy(fill.fee_asset, "USDT", sizeof(fill.fee_asset) - 1);
    fill.exchange_ts_ns = 10;
    snapshot.fills.push(fill);

    ReconciledPosition pos;
    std::strncpy(pos.symbol, "BTCUSDT", sizeof(pos.symbol) - 1);
    pos.quantity = 0.2;
    pos.avg_entry_price = 50000.0;
    pos.leverage = 1.0;
    snapshot.positions.push(pos);

    return snapshot;
}

TEST(LiveSessionAccountingTest, ProducesVenueAndGlobalMetrics) {
    LiveSessionAccounting accounting;
    accounting.configure_venue(Exchange::BINANCE, 10000.0, 250.0);

    const ReconciliationSnapshot snapshot = make_spot_snapshot();
    accounting.ingest_reconciliation(Exchange::BINANCE, snapshot, "BTCUSDT", 51000.0);

    const auto venue = accounting.venue_metrics(Exchange::BINANCE);
    EXPECT_DOUBLE_EQ(venue.start_equity, 10000.0);
    EXPECT_DOUBLE_EQ(venue.end_equity, 10101.0);
    EXPECT_DOUBLE_EQ(venue.unrealized_pnl, 200.0);
    EXPECT_DOUBLE_EQ(venue.fees, 2.0);
    EXPECT_DOUBLE_EQ(venue.net_pnl, 101.0);
    EXPECT_DOUBLE_EQ(venue.realized_pnl, -97.0);

    const auto global = accounting.global_metrics();
    EXPECT_DOUBLE_EQ(global.start_equity, 10000.0);
    EXPECT_DOUBLE_EQ(global.end_equity, 10101.0);
    EXPECT_DOUBLE_EQ(global.fees, 2.0);
    EXPECT_DOUBLE_EQ(global.unrealized_pnl, 200.0);
    EXPECT_DOUBLE_EQ(global.net_pnl, 101.0);
    EXPECT_DOUBLE_EQ(global.realized_pnl, -97.0);
    EXPECT_NEAR(global.return_pct, 1.01, 1e-9);
}

TEST(LiveSessionAccountingTest, SpotAffordabilityUsesBalancesAndBuffers) {
    LiveSessionAccounting accounting;
    accounting.configure_venue(Exchange::BINANCE, 10000.0, 250.0);

    ReconciliationSnapshot snapshot;
    ReconciledBalance usdt;
    std::strncpy(usdt.asset, "USDT", sizeof(usdt.asset) - 1);
    usdt.total = 10000.0;
    usdt.available = 1200.0;
    snapshot.balances.push(usdt);

    ReconciledBalance btc;
    std::strncpy(btc.asset, "BTC", sizeof(btc.asset) - 1);
    btc.total = 0.25;
    btc.available = 0.25;
    snapshot.balances.push(btc);

    accounting.ingest_reconciliation(Exchange::BINANCE, snapshot, "BTCUSDT", 50000.0);

    EXPECT_TRUE(accounting.can_afford_spot_order(Exchange::BINANCE, "BTCUSDT", Side::BID, 0.01,
                                                 50000.0, 5.0));
    EXPECT_FALSE(accounting.can_afford_spot_order(Exchange::BINANCE, "BTCUSDT", Side::BID, 0.02,
                                                  50000.0, 5.0));
    EXPECT_TRUE(accounting.can_afford_spot_order(Exchange::BINANCE, "BTCUSDT", Side::ASK, 0.2,
                                                 50000.0, 5.0));
    EXPECT_FALSE(accounting.can_afford_spot_order(Exchange::BINANCE, "BTCUSDT", Side::ASK, 0.3,
                                                  50000.0, 5.0));
}

TEST(LiveSessionAccountingTest, FuturesAffordabilityUsesCollateralAndLeverage) {
    LiveSessionAccounting accounting;
    accounting.configure_venue(Exchange::BINANCE, 10000.0, 250.0);

    ReconciliationSnapshot snapshot;
    ReconciledBalance usdt;
    std::strncpy(usdt.asset, "USDT", sizeof(usdt.asset) - 1);
    usdt.total = 10000.0;
    usdt.available = 3000.0;
    snapshot.balances.push(usdt);

    accounting.ingest_reconciliation(Exchange::BINANCE, snapshot, "BTCUSDT", 50000.0);

    EXPECT_TRUE(accounting.can_afford_futures_order(Exchange::BINANCE, 0.25, 50000.0, 5.0, 10.0));
    EXPECT_FALSE(accounting.can_afford_futures_order(Exchange::BINANCE, 0.75, 50000.0, 5.0, 10.0));
}

TEST(LiveSessionAccountingTest, DeduplicatesFeesAcrossRepeatedSnapshots) {
    LiveSessionAccounting accounting;
    accounting.configure_venue(Exchange::BINANCE, 10000.0, 0.0);

    ReconciliationSnapshot snapshot = make_spot_snapshot();
    accounting.ingest_reconciliation(Exchange::BINANCE, snapshot, "BTCUSDT", 50000.0);
    accounting.ingest_reconciliation(Exchange::BINANCE, snapshot, "BTCUSDT", 50000.0);

    const auto venue = accounting.venue_metrics(Exchange::BINANCE);
    EXPECT_DOUBLE_EQ(venue.fees, 2.0);
}

} 
