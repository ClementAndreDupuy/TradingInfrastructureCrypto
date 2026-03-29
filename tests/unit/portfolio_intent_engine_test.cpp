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
    cfg.negative_reversal_signal_bps = -8.0;
    cfg.deadband_signal_bps = 1.0;
    cfg.max_risk_score = 0.65;
    cfg.shock_enter_threshold = 0.70;
    cfg.shock_exit_threshold = 0.50;
    cfg.illiquid_enter_threshold = 0.65;
    cfg.illiquid_exit_threshold = 0.45;
    cfg.regime_persistence_ticks = 5;
    cfg.stale_inventory_ms = 10000;
    cfg.stale_signal_ms = 1500;
    cfg.max_basis_divergence_bps = 25.0;
    cfg.stale_inventory_alpha_hold_bps = 10.0;
    cfg.health_reduce_ratio = 0.50;
    cfg.long_only = false;
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

PortfolioIntentContext make_ctx(double spot_mid = 100.0, double futures_mid = 100.02,
                                int64_t signal_age_ms = 20) {
    PortfolioIntentContext ctx;
    ctx.spot_mid_price = spot_mid;
    ctx.futures_mid_price = futures_mid;
    ctx.signal_age_ms = signal_age_ms;
    return ctx;
}

TEST(PortfolioIntentEngineTest, PositiveAlphaProducesLongTarget) {
    PortfolioIntentEngine engine(make_cfg());
    const auto venues = make_venues();
    const auto alpha = make_alpha(8.0);
    const auto regime = make_regime();
    const auto ledger = make_ledger();

    const PortfolioIntent intent = engine.evaluate(alpha, regime, ledger, venues, make_ctx());

    EXPECT_GT(intent.target_global_position, 0.0);
    EXPECT_GT(intent.position_delta, 0.0);
    EXPECT_EQ(intent.primary_reason(), PortfolioIntentReasonCode::ALPHA_POSITIVE);
}

TEST(PortfolioIntentEngineTest, NegativeAlphaProducesShortTarget) {
    PortfolioIntentEngine engine(make_cfg());
    const auto venues = make_venues();
    const auto alpha = make_alpha(-8.0);

    const PortfolioIntent intent = engine.evaluate(alpha, make_regime(), make_ledger(), venues, make_ctx());

    EXPECT_LT(intent.target_global_position, 0.0);
    EXPECT_LT(intent.position_delta, 0.0);
    EXPECT_EQ(intent.primary_reason(), PortfolioIntentReasonCode::ALPHA_NEGATIVE);
}

TEST(PortfolioIntentEngineTest, DeadbandAlphaFlattensToNoTrade) {
    PortfolioIntentEngine engine(make_cfg());
    const auto venues = make_venues();

    const PortfolioIntent intent = engine.evaluate(make_alpha(0.2), make_regime(), make_ledger(0.1), venues, make_ctx());

    EXPECT_NEAR(intent.target_global_position, 0.0, 1e-9);
    EXPECT_LT(intent.position_delta, 0.0);
}

TEST(PortfolioIntentEngineTest, StaleSignalTriggersFlatten) {
    PortfolioIntentEngine engine(make_cfg());

    const PortfolioIntent intent = engine.evaluate(make_alpha(9.0), make_regime(), make_ledger(0.4), make_venues(),
                                                   make_ctx(100.0, 100.02, 2500));

    EXPECT_TRUE(intent.flatten_now);
    EXPECT_EQ(intent.primary_reason(), PortfolioIntentReasonCode::STALE_SIGNAL);
}

TEST(PortfolioIntentEngineTest, WideBasisTriggersFlatten) {
    PortfolioIntentEngine engine(make_cfg());

    const PortfolioIntent intent = engine.evaluate(make_alpha(9.0), make_regime(), make_ledger(0.4), make_venues(),
                                                   make_ctx(100.0, 101.0, 10));

    EXPECT_TRUE(intent.flatten_now);
    EXPECT_EQ(intent.primary_reason(), PortfolioIntentReasonCode::BASIS_TOO_WIDE);
}

TEST(PortfolioIntentEngineTest, TransitionClassesDeterministic) {
    PortfolioIntentEngine engine(make_cfg());
    const auto venues = make_venues();
    const auto regime = make_regime();
    const auto ctx = make_ctx();

    const PortfolioIntent flat_to_long = engine.evaluate(make_alpha(8.0), regime, make_ledger(0.0), venues, ctx);
    EXPECT_GT(flat_to_long.position_delta, 0.0);

    const PortfolioIntent flat_to_short = engine.evaluate(make_alpha(-8.0), regime, make_ledger(0.0), venues, ctx);
    EXPECT_LT(flat_to_short.position_delta, 0.0);

    const PortfolioIntent long_to_short = engine.evaluate(make_alpha(-8.0), regime, make_ledger(0.5), venues, ctx);
    EXPECT_LT(long_to_short.target_global_position, 0.0);
    EXPECT_LT(long_to_short.position_delta, -0.5);

    const PortfolioIntent short_to_long = engine.evaluate(make_alpha(8.0), regime, make_ledger(-0.5), venues, ctx);
    EXPECT_GT(short_to_long.target_global_position, 0.0);
    EXPECT_GT(short_to_long.position_delta, 0.5);

    const PortfolioIntent long_to_flat = engine.evaluate(make_alpha(0.2), regime, make_ledger(0.4), venues, ctx);
    EXPECT_NEAR(long_to_flat.target_global_position, 0.0, 1e-9);
    EXPECT_LT(long_to_flat.position_delta, 0.0);

    const PortfolioIntent short_to_flat = engine.evaluate(make_alpha(-0.2), regime, make_ledger(-0.4), venues, ctx);
    EXPECT_NEAR(short_to_flat.target_global_position, 0.0, 1e-9);
    EXPECT_GT(short_to_flat.position_delta, 0.0);
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
