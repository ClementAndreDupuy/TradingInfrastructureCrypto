#include "core/risk/arb_risk_manager.hpp"
#include <gtest/gtest.h>
#include <chrono>

using namespace trading;

// ── Helpers ──────────────────────────────────────────────────────────────────

static int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Build a default ArbRiskConfig with tight limits for deterministic tests.
static ArbRiskConfig test_config() {
    ArbRiskConfig cfg;
    cfg.max_abs_position_per_symbol  = 10.0;
    cfg.max_cross_exchange_exposure  = 20.0;
    cfg.max_notional_per_symbol      = 1'000'000.0;
    cfg.max_notional_per_exchange    = 5'000'000.0;
    cfg.max_portfolio_notional       = 10'000'000.0;
    cfg.max_drawdown_usd             = -100'000.0;
    cfg.max_orders_per_second        = 100;
    cfg.max_orders_per_minute        = 6000;
    cfg.min_spread_bps               = 2.0;
    cfg.min_profit_usd               = 0.01;
    cfg.max_book_age_ns              = 2'000'000'000LL;  // 2 seconds (generous for tests)
    cfg.max_open_arb_legs            = 10;
    cfg.max_price_deviation_bps      = 1000.0;  // 10% (generous for tests)
    cfg.circuit_breaker_count        = 5;
    cfg.circuit_breaker_loss_usd     = -10'000.0;
    return cfg;
}

// Build a fresh opportunity with a valid spread.
// buy on BINANCE at 50000, sell on KRAKEN at 50100 → 20 bps gross spread
static ArbOpportunity make_opp(const char* sym = "BTC",
                                double buy_price  = 50000.0,
                                double sell_price = 50100.0,
                                double qty        = 0.1) {
    ArbOpportunity opp;
    std::strncpy(opp.symbol, sym, 15);
    opp.buy_exchange  = Exchange::BINANCE;
    opp.sell_exchange = Exchange::KRAKEN;
    opp.buy_price     = buy_price;
    opp.sell_price    = sell_price;
    opp.quantity      = qty;
    opp.reference_price = (buy_price + sell_price) / 2.0;
    int64_t ts = now_ns();
    opp.local_ts_ns   = ts;
    opp.buy_book_ts_ns  = ts - 10'000'000;  // 10ms ago
    opp.sell_book_ts_ns = ts - 10'000'000;
    return opp;
}

// ── Kill Switch Tests ─────────────────────────────────────────────────────────

TEST(KillSwitch, InitiallyInactive) {
    KillSwitch ks;
    EXPECT_FALSE(ks.is_active());
}

TEST(KillSwitch, TriggerActivates) {
    KillSwitch ks;
    ks.trigger(KillReason::MANUAL);
    EXPECT_TRUE(ks.is_active());
    EXPECT_EQ(ks.get_reason(), KillReason::MANUAL);
}

TEST(KillSwitch, ResetDeactivates) {
    KillSwitch ks;
    ks.trigger(KillReason::DRAWDOWN);
    EXPECT_TRUE(ks.is_active());
    ks.reset();
    EXPECT_FALSE(ks.is_active());
}

TEST(KillSwitch, HeartbeatPreventsTimeout) {
    KillSwitch ks(500'000'000LL);  // 500ms timeout
    ks.heartbeat();
    EXPECT_TRUE(ks.check_heartbeat());
    EXPECT_FALSE(ks.is_active());
}

// ── evaluate_arb: Happy Path ──────────────────────────────────────────────────

TEST(ArbRiskManager, ApprovedOnValidOpportunity) {
    ArbRiskManager mgr(test_config());
    auto opp = make_opp();
    EXPECT_EQ(mgr.evaluate_arb(opp), RiskVerdict::APPROVED);
}

// ── evaluate_arb: Kill Switch ─────────────────────────────────────────────────

TEST(ArbRiskManager, RejectsWhenKillSwitchActive) {
    ArbRiskManager mgr(test_config());
    mgr.trigger_kill_switch();
    EXPECT_EQ(mgr.evaluate_arb(make_opp()), RiskVerdict::REJECTED_KILL_SWITCH);
}

TEST(ArbRiskManager, ApprovesAfterKillSwitchReset) {
    ArbRiskManager mgr(test_config());
    mgr.trigger_kill_switch();
    mgr.reset_kill_switch();
    EXPECT_EQ(mgr.evaluate_arb(make_opp()), RiskVerdict::APPROVED);
}

// ── evaluate_arb: Invalid Input ───────────────────────────────────────────────

TEST(ArbRiskManager, RejectsZeroPrice) {
    ArbRiskManager mgr(test_config());
    auto opp = make_opp();
    opp.buy_price = 0.0;
    EXPECT_EQ(mgr.evaluate_arb(opp), RiskVerdict::REJECTED_INVALID_INPUT);
}

TEST(ArbRiskManager, RejectsZeroQuantity) {
    ArbRiskManager mgr(test_config());
    auto opp = make_opp();
    opp.quantity = 0.0;
    EXPECT_EQ(mgr.evaluate_arb(opp), RiskVerdict::REJECTED_INVALID_INPUT);
}

TEST(ArbRiskManager, RejectsSameExchange) {
    ArbRiskManager mgr(test_config());
    auto opp = make_opp();
    opp.sell_exchange = Exchange::BINANCE;  // same as buy
    EXPECT_EQ(mgr.evaluate_arb(opp), RiskVerdict::REJECTED_INVALID_INPUT);
}

// ── evaluate_arb: Book Freshness ──────────────────────────────────────────────

TEST(ArbRiskManager, RejectsStaleBuyBook) {
    ArbRiskConfig cfg = test_config();
    cfg.max_book_age_ns = 100'000'000LL;  // 100ms
    ArbRiskManager mgr(cfg);

    auto opp = make_opp();
    opp.local_ts_ns    = now_ns();
    opp.buy_book_ts_ns = opp.local_ts_ns - 200'000'000LL;  // 200ms ago — stale
    EXPECT_EQ(mgr.evaluate_arb(opp), RiskVerdict::REJECTED_STALE_BOOK);
}

TEST(ArbRiskManager, RejectsStaleSellBook) {
    ArbRiskConfig cfg = test_config();
    cfg.max_book_age_ns = 100'000'000LL;
    ArbRiskManager mgr(cfg);

    auto opp = make_opp();
    opp.local_ts_ns     = now_ns();
    opp.sell_book_ts_ns = opp.local_ts_ns - 200'000'000LL;
    EXPECT_EQ(mgr.evaluate_arb(opp), RiskVerdict::REJECTED_STALE_BOOK);
}

// ── evaluate_arb: Flash Crash Guard ──────────────────────────────────────────

TEST(ArbRiskManager, RejectsFlashCrash) {
    ArbRiskConfig cfg = test_config();
    cfg.max_price_deviation_bps = 50.0;  // 0.5%
    ArbRiskManager mgr(cfg);

    auto opp = make_opp();
    opp.reference_price = 50000.0;
    opp.sell_price      = 52000.0;  // 4% above reference — reject
    EXPECT_EQ(mgr.evaluate_arb(opp), RiskVerdict::REJECTED_FLASH_CRASH);
}

// ── evaluate_arb: Spread / Profitability ─────────────────────────────────────

TEST(ArbRiskManager, RejectsSpreadTooSmall) {
    ArbRiskConfig cfg = test_config();
    cfg.min_spread_bps = 50.0;  // require 50 bps
    ArbRiskManager mgr(cfg);

    // 20 bps gross spread — below threshold
    auto opp = make_opp("BTC", 50000.0, 50100.0, 0.1);
    EXPECT_EQ(mgr.evaluate_arb(opp), RiskVerdict::REJECTED_SPREAD_TOO_SMALL);
}

TEST(ArbRiskManager, RejectsFeeAdjustedLoss) {
    ArbRiskConfig cfg = test_config();
    cfg.min_spread_bps   = 2.0;
    cfg.min_profit_usd   = 100.0;  // require $100 min profit
    ArbRiskManager mgr(cfg);

    auto opp = make_opp("BTC", 50000.0, 50100.0, 0.1);  // ~$10 gross profit
    EXPECT_EQ(mgr.evaluate_arb(opp), RiskVerdict::REJECTED_FEE_ADJUSTED_LOSS);
}

// ── evaluate_arb: Open Leg Limit ─────────────────────────────────────────────

TEST(ArbRiskManager, RejectsWhenMaxLegsReached) {
    ArbRiskConfig cfg = test_config();
    cfg.max_open_arb_legs = 2;
    ArbRiskManager mgr(cfg);

    auto opp = make_opp();
    ASSERT_EQ(mgr.evaluate_arb(opp), RiskVerdict::APPROVED);
    ASSERT_NE(mgr.open_leg(opp), 0u);

    ASSERT_EQ(mgr.evaluate_arb(opp), RiskVerdict::APPROVED);
    ASSERT_NE(mgr.open_leg(opp), 0u);

    // Third leg should be rejected
    EXPECT_EQ(mgr.evaluate_arb(opp), RiskVerdict::REJECTED_MAX_ARB_LEGS);
}

// ── evaluate_arb: Rate Limiting ───────────────────────────────────────────────

TEST(ArbRiskManager, RejectsOnRateLimitExceeded) {
    ArbRiskConfig cfg = test_config();
    cfg.max_orders_per_second = 3;
    ArbRiskManager mgr(cfg);

    int64_t ts = now_ns();
    auto opp = make_opp();
    opp.local_ts_ns     = ts;
    opp.buy_book_ts_ns  = ts - 10'000'000;
    opp.sell_book_ts_ns = ts - 10'000'000;

    // First 3 should be approved
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(mgr.evaluate_arb(opp), RiskVerdict::APPROVED) << "iteration " << i;
        mgr.open_leg(opp);  // consume the slot
    }

    // 4th in the same second must be rejected
    EXPECT_EQ(mgr.evaluate_arb(opp), RiskVerdict::REJECTED_RATE_LIMIT);
}

// ── evaluate_arb: Position Limits ────────────────────────────────────────────

TEST(ArbRiskManager, RejectsWhenPositionLimitExceeded) {
    ArbRiskConfig cfg = test_config();
    cfg.max_abs_position_per_symbol = 0.05;  // only 0.05 BTC allowed
    ArbRiskManager mgr(cfg);

    auto opp = make_opp("BTC", 50000.0, 50100.0, 0.1);  // 0.1 BTC > 0.05 limit
    EXPECT_EQ(mgr.evaluate_arb(opp), RiskVerdict::REJECTED_POSITION_LIMIT);
}

// ── evaluate_arb: Notional Limits ────────────────────────────────────────────

TEST(ArbRiskManager, RejectsNotionalPerSymbolExceeded) {
    ArbRiskConfig cfg = test_config();
    cfg.max_notional_per_symbol = 1000.0;  // $1,000 max per leg
    ArbRiskManager mgr(cfg);

    // 1 BTC at $50,000 = $50,000 notional > $1,000 limit
    auto opp = make_opp("BTC", 50000.0, 50100.0, 1.0);
    EXPECT_EQ(mgr.evaluate_arb(opp), RiskVerdict::REJECTED_NOTIONAL_PER_SYM);
}

TEST(ArbRiskManager, RejectsPortfolioNotionalExceeded) {
    ArbRiskConfig cfg = test_config();
    cfg.max_portfolio_notional = 100.0;  // tiny limit
    ArbRiskManager mgr(cfg);

    auto opp = make_opp("BTC", 50000.0, 50100.0, 0.1);  // $5,000 notional
    EXPECT_EQ(mgr.evaluate_arb(opp), RiskVerdict::REJECTED_PORTFOLIO_NOTIONAL);
}

// ── Leg Lifecycle ─────────────────────────────────────────────────────────────

TEST(ArbRiskManager, OpenLegIncreasesCount) {
    ArbRiskManager mgr(test_config());
    EXPECT_EQ(mgr.get_open_leg_count(), 0u);

    auto opp = make_opp();
    ASSERT_EQ(mgr.evaluate_arb(opp), RiskVerdict::APPROVED);
    uint64_t id = mgr.open_leg(opp);

    EXPECT_GT(id, 0u);
    EXPECT_EQ(mgr.get_open_leg_count(), 1u);
}

TEST(ArbRiskManager, BothLegsFillClosesLegAndRecordsPnl) {
    ArbRiskManager mgr(test_config());
    auto opp = make_opp("BTC", 50000.0, 50100.0, 1.0);

    ASSERT_EQ(mgr.evaluate_arb(opp), RiskVerdict::APPROVED);
    uint64_t id = mgr.open_leg(opp);
    ASSERT_GT(id, 0u);

    double pnl_before = mgr.get_portfolio_pnl();
    mgr.on_buy_filled(id,  50000.0, 1.0);
    mgr.on_sell_filled(id, 50100.0, 1.0);

    EXPECT_EQ(mgr.get_open_leg_count(), 0u);

    // Gross = $100, fees = (7.5 + 10) bps * $50000 * 1 = 17.5 bps ≈ $87.5
    // Net PnL should be positive
    double net_pnl = mgr.get_portfolio_pnl() - pnl_before;
    EXPECT_GT(net_pnl, 0.0);
    EXPECT_LT(net_pnl, 100.0);  // must be less than gross
}

TEST(ArbRiskManager, LegRejectedWithOrphanLogsAndReleases) {
    ArbRiskManager mgr(test_config());
    auto opp = make_opp();

    ASSERT_EQ(mgr.evaluate_arb(opp), RiskVerdict::APPROVED);
    uint64_t id = mgr.open_leg(opp);
    ASSERT_GT(id, 0u);

    // Simulate buy filled but sell rejected
    mgr.on_buy_filled(id, opp.buy_price, opp.quantity);
    mgr.on_leg_rejected(id);

    // Leg is released
    EXPECT_EQ(mgr.get_open_leg_count(), 0u);
}

// ── Circuit Breaker ───────────────────────────────────────────────────────────

TEST(ArbRiskManager, CircuitBreakerTripsAfterConsecutiveLosses) {
    ArbRiskConfig cfg = test_config();
    cfg.circuit_breaker_count    = 3;
    cfg.circuit_breaker_loss_usd = -1.0;  // any negative PnL counts
    ArbRiskManager mgr(cfg);

    // Simulate 3 back-to-back losing legs
    for (int i = 0; i < 3; ++i) {
        auto opp = make_opp("BTC", 50100.0, 50000.0, 0.1);  // inverted: buy high, sell low
        opp.sell_price      = 50150.0;  // need valid spread to pass evaluate_arb
        opp.buy_price       = 50000.0;
        opp.reference_price = 50075.0;
        ASSERT_EQ(mgr.evaluate_arb(opp), RiskVerdict::APPROVED);
        uint64_t id = mgr.open_leg(opp);
        // Fill with a loss: buy at 50000, sell at 49500 (below buy)
        mgr.on_buy_filled(id,  50000.0, 0.1);
        mgr.on_sell_filled(id, 49500.0, 0.1);
    }

    EXPECT_TRUE(mgr.is_circuit_breaker_active());
    EXPECT_EQ(mgr.evaluate_arb(make_opp()), RiskVerdict::REJECTED_CIRCUIT_BREAKER);
}

TEST(ArbRiskManager, CircuitBreakerResetsManually) {
    ArbRiskConfig cfg = test_config();
    cfg.circuit_breaker_count    = 2;
    cfg.circuit_breaker_loss_usd = -1.0;
    ArbRiskManager mgr(cfg);

    for (int i = 0; i < 2; ++i) {
        auto opp = make_opp();
        ASSERT_EQ(mgr.evaluate_arb(opp), RiskVerdict::APPROVED);
        uint64_t id = mgr.open_leg(opp);
        mgr.on_buy_filled(id,  50000.0, 0.1);
        mgr.on_sell_filled(id, 49500.0, 0.1);  // loss
    }

    EXPECT_TRUE(mgr.is_circuit_breaker_active());
    mgr.reset_circuit_breaker();
    EXPECT_FALSE(mgr.is_circuit_breaker_active());
}

// ── Drawdown Kill Switch ──────────────────────────────────────────────────────

TEST(ArbRiskManager, DrawdownTriggersKillSwitch) {
    ArbRiskConfig cfg = test_config();
    cfg.max_drawdown_usd         = -20.0;    // $20 drawdown limit (leg loses ~$28.75)
    cfg.circuit_breaker_count    = 1000;     // disable circuit breaker for this test
    cfg.circuit_breaker_loss_usd = -1e9;
    ArbRiskManager mgr(cfg);

    // Open and fill a losing leg that exceeds drawdown
    auto opp = make_opp();
    ASSERT_EQ(mgr.evaluate_arb(opp), RiskVerdict::APPROVED);
    uint64_t id = mgr.open_leg(opp);
    mgr.on_buy_filled(id,  50000.0, 1.0);
    mgr.on_sell_filled(id, 49800.0, 1.0);  // net ~-$28.75 exceeds -$20.0 limit

    EXPECT_TRUE(mgr.is_kill_switch_active());
    EXPECT_EQ(mgr.kill_switch().get_reason(), KillReason::DRAWDOWN);
}

// ── Position Tracking ─────────────────────────────────────────────────────────

TEST(ArbRiskManager, PositionUpdatedOnFills) {
    ArbRiskManager mgr(test_config());

    auto opp = make_opp("ETH", 3000.0, 3030.0, 2.0);
    ASSERT_EQ(mgr.evaluate_arb(opp), RiskVerdict::APPROVED);
    uint64_t id = mgr.open_leg(opp);

    mgr.on_buy_filled(id,  3000.0, 2.0);
    mgr.on_sell_filled(id, 3030.0, 2.0);

    // After full round-trip fills, net position should be near zero
    double pos_binance = mgr.get_position(Exchange::BINANCE, "ETH");
    double pos_kraken  = mgr.get_position(Exchange::KRAKEN,  "ETH");
    EXPECT_DOUBLE_EQ(pos_binance,  2.0);   // long from buy fill
    EXPECT_DOUBLE_EQ(pos_kraken,  -2.0);   // short from sell fill
}

// ── Metrics ───────────────────────────────────────────────────────────────────

TEST(ArbRiskManager, VerdictToStringCoversAllCodes) {
    // Smoke test: all enum values must return non-empty strings
    for (uint8_t i = 0; i <= 14; ++i) {
        const char* s = verdict_to_string(static_cast<RiskVerdict>(i));
        EXPECT_NE(s, nullptr);
        EXPECT_GT(std::strlen(s), 0u);
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
