#include "core/execution/common/portfolio/portfolio_intent_engine.hpp"
#include "core/execution/common/orders/parent_order_manager.hpp"

#include <gtest/gtest.h>

namespace trading {
namespace {

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
    PortfolioIntentEngine engine;
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
    PortfolioIntentEngine engine;
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
    PortfolioIntentEngine engine;
    auto venues = make_venues();
    venues[3].healthy = false;
    const auto alpha = make_alpha(7.0, 0.20, 1.0, 10);
    const auto regime = make_regime(0.05, 0.70);
    const auto ledger = make_ledger(0.60, 2500);

    const PortfolioIntent intent = engine.evaluate(alpha, regime, ledger, venues);

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

TEST(PortfolioIntentEngineTest, NoHealthyVenuesTriggersFlattenReason) {
    PortfolioIntentEngine engine;
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
            manager.update_target(0.40, ShadowUrgency::AGGRESSIVE, now);
    ASSERT_TRUE(created.plan.active());
    EXPECT_DOUBLE_EQ(created.plan.remaining_qty, 0.40);

    const ParentPlanUpdateResult updated =
            manager.update_target(0.02, ShadowUrgency::AGGRESSIVE, now + std::chrono::milliseconds(1));
    ASSERT_TRUE(updated.plan.active());
    EXPECT_EQ(updated.action, ParentPlanAction::UPDATED);
    EXPECT_DOUBLE_EQ(updated.plan.total_qty, 0.02);
    EXPECT_DOUBLE_EQ(updated.plan.remaining_qty, 0.02);
}

} 
}
