#include "core/execution/smart_order_router.hpp"

#include <gtest/gtest.h>

#include <array>

namespace trading {

TEST(SmartOrderRouterTest, SplitsAcrossBestVenuesByScore) {
    SmartOrderRouter sor;
    std::array<VenueQuote, SmartOrderRouter::MAX_VENUES> venues{{
        {Exchange::BINANCE, 100.0, 100.2, 0.20, 5.0, 0.2, 0.2, 0.75, 0.20, 0.2, true},
        {Exchange::KRAKEN, 100.0, 100.1, 0.15, 4.0, 0.2, 0.2, 0.70, 0.20, 0.2, true},
        {Exchange::OKX, 100.0, 100.0, 0.10, 7.0, 0.2, 0.2, 0.65, 0.20, 0.2, true},
        {Exchange::COINBASE, 100.0, 100.3, 1.00, 5.0, 0.2, 0.2, 0.70, 0.20, 0.2, true},
    }};

    RoutingDecision d = sor.route(Side::BID, 0.25, venues);

    ASSERT_EQ(d.child_count, 2u);
    EXPECT_EQ(d.children[0].exchange, Exchange::OKX);
    EXPECT_NEAR(d.children[0].quantity, 0.10, 1e-12);
    EXPECT_EQ(d.children[1].exchange, Exchange::KRAKEN);
    EXPECT_NEAR(d.children[1].quantity, 0.15, 1e-12);
}

TEST(SmartOrderRouterTest, SkipsUnhealthyOrEmptyVenues) {
    SmartOrderRouter sor;
    std::array<VenueQuote, SmartOrderRouter::MAX_VENUES> venues{{
        {Exchange::BINANCE, 100.0, 100.1, 0.0, 5.0, 0.1, 0.1, 0.70, 0.10, 0.1, true},
        {Exchange::KRAKEN, 100.0, 100.2, 1.0, 4.0, 0.1, 0.1, 0.70, 0.10, 0.1, false},
        {Exchange::OKX, 100.0, 100.0, 1.0, 4.0, 0.1, 0.1, 0.70, 0.10, 0.1, true},
        {Exchange::COINBASE, 100.0, 100.3, 1.0, 2.0, 0.1, 0.1, 0.70, 0.10, 0.1, true},
    }};

    RoutingDecision d = sor.route(Side::BID, 0.5, venues);

    ASSERT_EQ(d.child_count, 1u);
    EXPECT_EQ(d.children[0].exchange, Exchange::OKX);
    EXPECT_NEAR(d.children[0].quantity, 0.5, 1e-12);
}

TEST(SmartOrderRouterTest, AlphaGateBlocksHighRiskAndScalesQty) {
    SmartOrderRouter sor;
    std::array<VenueQuote, SmartOrderRouter::MAX_VENUES> venues{{
        {Exchange::BINANCE, 100.0, 100.1, 10.0, 2.0, 0.1, 0.1, 0.80, 0.10, 0.1, true},
        {Exchange::KRAKEN, 100.0, 100.2, 10.0, 4.0, 0.1, 0.1, 0.80, 0.10, 0.1, true},
        {Exchange::OKX, 100.0, 100.3, 10.0, 4.0, 0.1, 0.1, 0.80, 0.10, 0.1, true},
        {Exchange::COINBASE, 100.0, 100.4, 10.0, 4.0, 0.1, 0.1, 0.80, 0.10, 0.1, true},
    }};

    AlphaSignal blocked;
    blocked.signal_bps = 10.0;
    blocked.risk_score = 0.80;
    RoutingDecision blocked_decision = sor.route_with_alpha(Side::BID, 1.0, blocked, venues);
    EXPECT_TRUE(blocked_decision.blocked_by_alpha);
    EXPECT_EQ(blocked_decision.child_count, 0u);

    AlphaSignal allowed;
    allowed.signal_bps = 5.0;
    allowed.risk_score = 0.20;
    RoutingConstraints cfg;
    cfg.alpha_qty_scale = 0.10;
    RoutingDecision allowed_decision = sor.route_with_alpha(Side::BID, 1.0, allowed, venues, cfg);
    ASSERT_FALSE(allowed_decision.blocked_by_alpha);
    ASSERT_EQ(allowed_decision.child_count, 1u);
    EXPECT_NEAR(allowed_decision.children[0].quantity, 1.5, 1e-12);
}

TEST(SmartOrderRouterTest, PrefersVenueWithBetterFillAndQueueDynamics) {
    SmartOrderRouter sor;
    std::array<VenueQuote, SmartOrderRouter::MAX_VENUES> venues{{
        {Exchange::BINANCE, 100.0, 100.0, 1.0, 5.0, 0.1, 0.1, 0.15, 4.00, 0.1, true},
        {Exchange::KRAKEN, 100.0, 100.0, 1.0, 5.0, 0.1, 0.1, 0.90, 0.10, 0.1, true},
        {Exchange::OKX, 100.0, 100.4, 1.0, 5.0, 0.1, 0.1, 0.90, 0.10, 0.1, true},
        {Exchange::COINBASE, 100.0, 100.5, 1.0, 5.0, 0.1, 0.1, 0.90, 0.10, 0.1, true},
    }};

    RoutingDecision d = sor.route(Side::BID, 0.50, venues);

    ASSERT_EQ(d.child_count, 1u);
    EXPECT_EQ(d.children[0].exchange, Exchange::KRAKEN);
    EXPECT_NEAR(d.children[0].quantity, 0.50, 1e-12);
}

TEST(SmartOrderRouterTest, HighToxicityRegimeAmplifiesAdverseSelectionPenalty) {
    SmartOrderRouter sor;
    std::array<VenueQuote, SmartOrderRouter::MAX_VENUES> venues{{
        {Exchange::BINANCE, 100.0, 100.0, 1.0, 1.0, 0.1, 0.1, 0.90, 0.10, 4.0, true},
        {Exchange::KRAKEN, 100.0, 100.1, 1.0, 1.0, 0.1, 0.1, 0.90, 0.10, 0.3, true},
        {Exchange::OKX, 100.0, 100.3, 1.0, 1.0, 0.1, 0.1, 0.90, 0.10, 3.0, true},
        {Exchange::COINBASE, 100.0, 100.4, 1.0, 1.0, 0.1, 0.1, 0.90, 0.10, 2.0, true},
    }};

    RoutingDecision d = sor.route(Side::BID, 0.25, venues);

    ASSERT_EQ(d.child_count, 1u);
    EXPECT_EQ(d.children[0].exchange, Exchange::KRAKEN);
}

} // namespace trading
