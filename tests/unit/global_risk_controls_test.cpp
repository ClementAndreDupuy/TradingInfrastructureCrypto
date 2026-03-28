#include "core/risk/global_risk_controls.hpp"
#include "core/risk/risk_config_loader.hpp"

#include <gtest/gtest.h>

namespace {

using namespace trading;

TEST(GlobalRiskControlsTest, EnforcesGrossAndVenueCaps) {
    KillSwitch kill_switch;
    GlobalRiskConfig cfg;
    cfg.max_gross_notional = 1000.0;
    cfg.max_net_notional = 800.0;
    cfg.max_symbol_concentration = 1.0;
    cfg.max_venue_notional = 600.0;
    cfg.max_cross_venue_net_notional = 800.0;
    cfg.kill_on_breach = false;

    GlobalRiskControls controls(cfg, kill_switch);

    EXPECT_EQ(controls.commit_order(Exchange::BINANCE, "BTCUSDT", 500.0),
              GlobalRiskCheckResult::OK);
    EXPECT_EQ(controls.commit_order(Exchange::BINANCE, "ETHUSDT", 200.0),
              GlobalRiskCheckResult::VENUE_CAP);
    EXPECT_EQ(controls.commit_order(Exchange::OKX, "BTCUSDT", 600.0),
              GlobalRiskCheckResult::GROSS_NOTIONAL_CAP);
}

TEST(GlobalRiskControlsTest, EnforcesConcentrationAndCrossVenueNetting) {
    KillSwitch kill_switch;
    GlobalRiskConfig cfg;
    cfg.max_gross_notional = 1000.0;
    cfg.max_net_notional = 1000.0;
    cfg.max_symbol_concentration = 0.7;
    cfg.max_venue_notional = 1000.0;
    cfg.max_cross_venue_net_notional = 250.0;
    cfg.kill_on_breach = false;

    GlobalRiskControls controls(cfg, kill_switch);

    EXPECT_EQ(controls.commit_order(Exchange::BINANCE, "BTCUSDT", 300.0),
              GlobalRiskCheckResult::OK);
    EXPECT_EQ(controls.check_order(Exchange::OKX, "BTCUSDT", 100.0),
              GlobalRiskCheckResult::CONCENTRATION_CAP);
    EXPECT_EQ(controls.commit_order(Exchange::COINBASE, "ETHUSDT", 200.0),
              GlobalRiskCheckResult::OK);
    EXPECT_EQ(controls.check_order(Exchange::KRAKEN, "ETHUSDT", 100.0),
              GlobalRiskCheckResult::CROSS_VENUE_NETTING_CAP);
}

TEST(GlobalRiskControlsTest, DoesNotTripConcentrationWhenPortfolioHasSingleSymbol) {
    KillSwitch kill_switch;
    GlobalRiskConfig cfg;
    cfg.max_gross_notional = 1000.0;
    cfg.max_net_notional = 1000.0;
    cfg.max_symbol_concentration = 0.3;
    cfg.max_venue_notional = 1000.0;
    cfg.max_cross_venue_net_notional = 1000.0;
    cfg.kill_on_breach = false;

    GlobalRiskControls controls(cfg, kill_switch);

    EXPECT_EQ(controls.commit_order(Exchange::KRAKEN, "SOLUSDT", 34.9), GlobalRiskCheckResult::OK);
    EXPECT_EQ(controls.check_order(Exchange::OKX, "SOLUSDT", 34.9), GlobalRiskCheckResult::OK);
}

TEST(GlobalRiskControlsTest, LoadsGlobalLimitsFromConfigFile) {
    RiskRuntimeConfig cfg;
    ASSERT_TRUE(RiskConfigLoader::load("config/live/risk.yaml", cfg));

    EXPECT_EQ(cfg.circuit_breaker.max_orders_per_second, 5);
    EXPECT_DOUBLE_EQ(cfg.global_risk.max_gross_notional, 13000.0);
    EXPECT_DOUBLE_EQ(cfg.global_risk.max_venue_notional, 6500.0);
    EXPECT_NEAR(cfg.global_risk.max_symbol_concentration, 0.30, 1e-12);
    EXPECT_DOUBLE_EQ(cfg.global_risk.max_cross_venue_net_notional, 2500.0);
    EXPECT_DOUBLE_EQ(cfg.target_range_usd, 50.0);
    EXPECT_TRUE(cfg.futures_risk.enabled);
    EXPECT_DOUBLE_EQ(cfg.futures_risk.max_projected_funding_cost_bps, 12.0);
    EXPECT_DOUBLE_EQ(cfg.futures_risk.funding_cost_scale_start_bps, 4.0);
    EXPECT_DOUBLE_EQ(cfg.futures_risk.min_funding_scale, 0.4);
    EXPECT_DOUBLE_EQ(cfg.futures_risk.max_mark_index_divergence_bps, 30.0);
    EXPECT_DOUBLE_EQ(cfg.futures_risk.max_maintenance_margin_ratio, 0.65);
    EXPECT_DOUBLE_EQ(cfg.futures_risk.default_max_leverage, 8.0);
    ASSERT_GE(cfg.futures_risk.symbol_limit_count, static_cast<size_t>(2));
}

TEST(GlobalRiskControlsTest, RejectsFuturesExposureOnLeverageAndMaintenanceCaps) {
    KillSwitch kill_switch;
    GlobalRiskConfig cfg;
    cfg.max_gross_notional = 100000.0;
    cfg.max_net_notional = 100000.0;
    cfg.max_symbol_concentration = 1.0;
    cfg.max_venue_notional = 100000.0;
    cfg.max_cross_venue_net_notional = 100000.0;
    cfg.kill_on_breach = false;

    FuturesRiskGateConfig futures_cfg;
    futures_cfg.enabled = true;
    futures_cfg.default_max_leverage = 5.0;
    futures_cfg.max_maintenance_margin_ratio = 0.5;

    GlobalRiskControls controls(cfg, futures_cfg, kill_switch);

    FuturesRiskContext high_maintenance;
    high_maintenance.collateral_notional = 1000.0;
    high_maintenance.current_abs_notional = 200.0;
    high_maintenance.maintenance_margin_ratio = 0.8;
    high_maintenance.mark_price = 50000.0;
    high_maintenance.index_price = 49990.0;
    high_maintenance.funding_rate_bps = 1.0;
    high_maintenance.hours_to_funding = 2.0;

    double scaled_notional = 0.0;
    EXPECT_EQ(controls.check_futures_order(Exchange::BINANCE, "BTCUSDT", 100.0, high_maintenance,
                                           scaled_notional),
              GlobalRiskCheckResult::FUTURES_MAINTENANCE_MARGIN_CAP);

    FuturesRiskContext high_leverage = high_maintenance;
    high_leverage.maintenance_margin_ratio = 0.2;
    high_leverage.current_abs_notional = 4950.0;
    EXPECT_EQ(controls.check_futures_order(Exchange::BINANCE, "BTCUSDT", 100.0, high_leverage,
                                           scaled_notional),
              GlobalRiskCheckResult::FUTURES_LEVERAGE_CAP);
}

TEST(GlobalRiskControlsTest, AppliesFundingScaleAndRejectsDivergence) {
    KillSwitch kill_switch;
    GlobalRiskConfig cfg;
    cfg.max_gross_notional = 100000.0;
    cfg.max_net_notional = 100000.0;
    cfg.max_symbol_concentration = 1.0;
    cfg.max_venue_notional = 100000.0;
    cfg.max_cross_venue_net_notional = 100000.0;
    cfg.kill_on_breach = false;

    FuturesRiskGateConfig futures_cfg;
    futures_cfg.enabled = true;
    futures_cfg.default_max_leverage = 20.0;
    futures_cfg.max_maintenance_margin_ratio = 0.9;
    futures_cfg.max_mark_index_divergence_bps = 20.0;
    futures_cfg.funding_cost_scale_start_bps = 4.0;
    futures_cfg.max_projected_funding_cost_bps = 10.0;
    futures_cfg.min_funding_scale = 0.5;

    GlobalRiskControls controls(cfg, futures_cfg, kill_switch);

    FuturesRiskContext funding_ctx;
    funding_ctx.collateral_notional = 2000.0;
    funding_ctx.current_abs_notional = 100.0;
    funding_ctx.maintenance_margin_ratio = 0.1;
    funding_ctx.mark_price = 50000.0;
    funding_ctx.index_price = 50000.0;
    funding_ctx.funding_rate_bps = 6.0;
    funding_ctx.hours_to_funding = 8.0;

    double scaled_notional = 0.0;
    EXPECT_EQ(controls.check_futures_order(Exchange::BINANCE, "BTCUSDT", 100.0, funding_ctx,
                                           scaled_notional),
              GlobalRiskCheckResult::OK);
    EXPECT_NEAR(scaled_notional, 83.3333333333, 1e-6);

    FuturesRiskContext divergence_ctx = funding_ctx;
    divergence_ctx.mark_price = 50150.0;
    divergence_ctx.index_price = 50000.0;
    EXPECT_EQ(controls.check_futures_order(Exchange::BINANCE, "BTCUSDT", 100.0, divergence_ctx,
                                           scaled_notional),
              GlobalRiskCheckResult::FUTURES_MARK_INDEX_DIVERGENCE_CAP);

    FuturesRiskContext funding_reject_ctx = funding_ctx;
    funding_reject_ctx.funding_rate_bps = 12.0;
    EXPECT_EQ(controls.check_futures_order(Exchange::BINANCE, "BTCUSDT", 100.0, funding_reject_ctx,
                                           scaled_notional),
              GlobalRiskCheckResult::FUTURES_FUNDING_COST_CAP);
}

} // namespace
