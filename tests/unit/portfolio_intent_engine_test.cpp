#include "core/execution/common/portfolio/portfolio_intent_engine.hpp"
#include "core/execution/common/orders/parent_order_manager.hpp"

#include <gtest/gtest.h>

namespace trading {
namespace {

PortfolioIntentConfig make_cfg() {
    PortfolioIntentConfig cfg{};
    cfg.max_position = 0.80;
    cfg.min_entry_signal_bps = 2.0;
    cfg.alpha_exit_buffer_bps = 0.75;
    cfg.negative_reversal_signal_bps = -1.0;
    cfg.max_risk_score = 0.65;
    cfg.shock_enter_threshold = 0.70;
    cfg.shock_exit_threshold = 0.50;
    cfg.illiquid_enter_threshold = 0.65;
    cfg.illiquid_exit_threshold = 0.45;
    cfg.regime_persistence_ticks = 5;
    cfg.stale_inventory_ms = 10000;
    cfg.stale_inventory_alpha_hold_bps = 10.0;
    cfg.health_reduce_ratio = 0.50;
    cfg.long_only = true;
    return cfg;
}

PositionLedgerSnapshot make_ledger(double position = 0.0, int64_t inventory_age_ms = 0) {
    PositionLedgerSnapshot snapshot;
    snapshot.global_position = position;
    snapshot.oldest_inventory_age_ms = inventory_age_ms;
    return snapshot;
}

std::array<VenueQuote, SmartOrderRouter::MAX_VENUES> make_venues(bool healthy = true) {
    std::array<VenueQuote, SmartOrderRouter::MAX_VENUES> venues{};
    venues[0] = {Exchange::BINANCE, 100.0, 100.5, 4.0, 5.0, 0.5, 0.4, 0.80, 0.10, 0.30, healthy, true};
    venues[1] = {Exchange::KRAKEN, 100.0, 100.5, 4.0, 4.0, 0.7, 0.4, 0.75, 0.10, 0.35, healthy, true};
    venues[2] = {Exchange::OKX, 100.0, 100.5, 4.0, 3.0, 0.4, 0.3, 0.82, 0.10, 0.20, healthy, true};
    venues[3] = {Exchange::COINBASE, 100.0, 100.5, 4.0, 6.0, 0.8, 0.5, 0.70, 0.10, 0.40, healthy, true};
    return venues;
}

AlphaSignal make_alpha(double signal_bps = 8.0, double risk_score = 0.20,
                       double size_fraction = 0.80, int64_t horizon_ticks = 6) {
    AlphaSignal signal;
    signal.signal_bps = signal_bps;
    signal.risk_score = risk_score;
    signal.size_fraction = size_fraction;
    signal.horizon_ticks = horizon_ticks;
    return signal;
}

RegimeSignal make_regime(double shock = 0.05, double illiquid = 0.05) {
    RegimeSignal regime;
    regime.p_calm = 0.80;
    regime.p_trending = 0.10;
    regime.p_shock = shock;
    regime.p_illiquid = illiquid;
    return regime;
}

TEST(PortfolioIntentEngineTest, PositiveAlphaProducesDeterministicTargetPosition) {
    PortfolioIntentEngine engine(make_cfg());
    const auto venues = make_venues();
    const auto alpha = make_alpha();
    const auto regime = make_regime();
    const auto ledger = make_ledger();

    const PortfolioIntent first = engine.evaluate(alpha, regime, ledger, venues);
    const PortfolioIntent second = engine.evaluate(alpha, regime, ledger, venues);

    EXPECT_DOUBLE_EQ(first.target_global_position, second.target_global_position);
    EXPECT_DOUBLE_EQ(first.position_delta, second.position_delta);
    EXPECT_EQ(first.urgency, second.urgency);
    ASSERT_GE(first.reason_count, 1u);
    EXPECT_EQ(first.primary_reason(), PortfolioIntentReasonCode::ALPHA_POSITIVE);
    EXPECT_GT(first.target_global_position, 0.0);
}

TEST(PortfolioIntentEngineTest, NegativeReversalFlattensOpenLongAggressively) {
    PortfolioIntentEngine engine(make_cfg());
    const auto venues = make_venues();
    const auto alpha = make_alpha(-4.5, 0.20, 0.80, 2);
    const auto regime = make_regime();
    const auto ledger = make_ledger(0.50, 200);

    const PortfolioIntent intent = engine.evaluate(alpha, regime, ledger, venues);

    EXPECT_DOUBLE_EQ(intent.target_global_position, 0.0);
    EXPECT_DOUBLE_EQ(intent.position_delta, -0.50);
    EXPECT_TRUE(intent.flatten_now);
    EXPECT_EQ(intent.urgency, ShadowUrgency::AGGRESSIVE);
    EXPECT_EQ(intent.primary_reason(), PortfolioIntentReasonCode::NEGATIVE_REVERSAL);
}

TEST(PortfolioIntentEngineTest, IlliquidRegimeAndStaleInventoryReduceTarget) {
    PortfolioIntentEngine engine(make_cfg());
    auto venues = make_venues();
    venues[3].healthy = false;
    const auto alpha = make_alpha(7.0, 0.20, 1.0, 10);
    const auto regime = make_regime(0.05, 0.70);
    const auto ledger = make_ledger(0.60, 12000);

    PortfolioIntent intent;
    for (int i = 0; i < 5; ++i)
        intent = engine.evaluate(alpha, regime, ledger, venues);

    EXPECT_LT(intent.target_global_position, 0.60);
    EXPECT_FALSE(intent.flatten_now);
    EXPECT_EQ(intent.urgency, ShadowUrgency::AGGRESSIVE);
    bool saw_illiquid = false;
    bool saw_stale = false;
    bool saw_health = false;
    for (size_t i = 0; i < intent.reason_count; ++i) {
        saw_illiquid |= intent.reason_codes[i] == PortfolioIntentReasonCode::ILLIQUID_REGIME;
        saw_stale |= intent.reason_codes[i] == PortfolioIntentReasonCode::STALE_INVENTORY;
        saw_health |= intent.reason_codes[i] == PortfolioIntentReasonCode::HEALTH_DEGRADED;
    }
    EXPECT_TRUE(saw_illiquid);
    EXPECT_TRUE(saw_stale);
    EXPECT_TRUE(saw_health);
}

TEST(PortfolioIntentEngineTest, RegimeHysteresisPreventsSingleTickFlips) {
    PortfolioIntentEngine engine(make_cfg());
    const auto venues = make_venues();
    const auto alpha  = make_alpha(8.0, 0.20, 0.80, 6);
    const auto ledger = make_ledger(0.50, 100);

    const auto below_enter = make_regime(0.05, 0.60);
    for (int i = 0; i < 10; ++i)
        engine.evaluate(alpha, below_enter, ledger, venues);
    {
        const PortfolioIntent intent = engine.evaluate(alpha, below_enter, ledger, venues);
        bool saw_illiquid = false;
        for (size_t i = 0; i < intent.reason_count; ++i)
            saw_illiquid |= intent.reason_codes[i] == PortfolioIntentReasonCode::ILLIQUID_REGIME;
        EXPECT_FALSE(saw_illiquid);
    }

    const auto above_enter = make_regime(0.05, 0.70);
    for (int i = 0; i < 4; ++i)
        engine.evaluate(alpha, above_enter, ledger, venues);
    {
        const PortfolioIntent intent = engine.evaluate(alpha, below_enter, ledger, venues);
        bool saw_illiquid = false;
        for (size_t i = 0; i < intent.reason_count; ++i)
            saw_illiquid |= intent.reason_codes[i] == PortfolioIntentReasonCode::ILLIQUID_REGIME;
        EXPECT_FALSE(saw_illiquid);
    }

    for (int i = 0; i < 5; ++i)
        engine.evaluate(alpha, above_enter, ledger, venues);
    {
        const PortfolioIntent intent = engine.evaluate(alpha, above_enter, ledger, venues);
        bool saw_illiquid = false;
        for (size_t i = 0; i < intent.reason_count; ++i)
            saw_illiquid |= intent.reason_codes[i] == PortfolioIntentReasonCode::ILLIQUID_REGIME;
        EXPECT_TRUE(saw_illiquid);
    }

    const auto in_band = make_regime(0.05, 0.55);
    for (int i = 0; i < 10; ++i)
        engine.evaluate(alpha, in_band, ledger, venues);
    {
        const PortfolioIntent intent = engine.evaluate(alpha, in_band, ledger, venues);
        bool saw_illiquid = false;
        for (size_t i = 0; i < intent.reason_count; ++i)
            saw_illiquid |= intent.reason_codes[i] == PortfolioIntentReasonCode::ILLIQUID_REGIME;
        EXPECT_TRUE(saw_illiquid);
    }

    const auto below_exit = make_regime(0.05, 0.30);
    for (int i = 0; i < 5; ++i)
        engine.evaluate(alpha, below_exit, ledger, venues);
    {
        const PortfolioIntent intent = engine.evaluate(alpha, below_exit, ledger, venues);
        bool saw_illiquid = false;
        for (size_t i = 0; i < intent.reason_count; ++i)
            saw_illiquid |= intent.reason_codes[i] == PortfolioIntentReasonCode::ILLIQUID_REGIME;
        EXPECT_FALSE(saw_illiquid);
    }
}

TEST(PortfolioIntentEngineTest, NoHealthyVenuesTriggersFlattenReason) {
    PortfolioIntentEngine engine(make_cfg());
    const auto venues = make_venues(false);
    const auto alpha = make_alpha();
    const auto regime = make_regime();
    const auto ledger = make_ledger(0.40, 100);

    const PortfolioIntent intent = engine.evaluate(alpha, regime, ledger, venues);

    EXPECT_TRUE(intent.flatten_now);
    EXPECT_DOUBLE_EQ(intent.target_global_position, 0.0);
    EXPECT_DOUBLE_EQ(intent.position_delta, -0.40);
    EXPECT_EQ(intent.primary_reason(), PortfolioIntentReasonCode::NO_HEALTHY_VENUES);
}

TEST(ParentOrderManagerTest, SmallerSameDirectionTargetCapsRemainingQty) {
    ParentOrderManager manager;
    const auto now = std::chrono::steady_clock::now();

    const ParentPlanUpdateResult created =
            manager.update_target(0.40, ShadowUrgency::AGGRESSIVE, now, std::chrono::milliseconds(0));
    ASSERT_TRUE(created.plan.active());
    EXPECT_DOUBLE_EQ(created.plan.remaining_qty, 0.40);

    const ParentPlanUpdateResult updated =
            manager.update_target(0.02, ShadowUrgency::AGGRESSIVE, now + std::chrono::milliseconds(1), std::chrono::milliseconds(0));
    ASSERT_TRUE(updated.plan.active());
    EXPECT_EQ(updated.action, ParentPlanAction::UPDATED);
    EXPECT_DOUBLE_EQ(updated.plan.total_qty, 0.02);
    EXPECT_DOUBLE_EQ(updated.plan.remaining_qty, 0.02);
}

}
}
