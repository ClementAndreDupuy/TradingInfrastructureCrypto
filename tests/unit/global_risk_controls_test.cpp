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
    EXPECT_EQ(controls.check_order(Exchange::KRAKEN, "ETHUSDT", 300.0),
              GlobalRiskCheckResult::CROSS_VENUE_NETTING_CAP);
}

TEST(GlobalRiskControlsTest, LoadsGlobalLimitsFromConfigFile) {
    RiskRuntimeConfig cfg;
    ASSERT_TRUE(RiskConfigLoader::load("config/dev/risk.yaml", cfg));

    EXPECT_EQ(cfg.circuit_breaker.max_orders_per_second, 10);
    EXPECT_DOUBLE_EQ(cfg.global_risk.max_gross_notional, 500000.0);
    EXPECT_DOUBLE_EQ(cfg.global_risk.max_venue_notional, 250000.0);
    EXPECT_NEAR(cfg.global_risk.max_symbol_concentration, 0.35, 1e-12);
    EXPECT_DOUBLE_EQ(cfg.global_risk.max_cross_venue_net_notional, 75000.0);
}

} // namespace
