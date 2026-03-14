#include "core/execution/smart_order_router.hpp"

#include <gtest/gtest.h>

#include <array>

namespace trading {

TEST(SmartOrderRouterTest, SplitsAcrossBestVenuesByScore) {
    SmartOrderRouter sor;
    std::array<VenueQuote, SmartOrderRouter::MAX_VENUES> venues{ {
        {Exchange::BINANCE, 100.0, 100.2, 0.20, 5.0, 0.2, 0.2, true},
        {Exchange::KRAKEN, 100.0, 100.1, 0.15, 4.0, 0.2, 0.2, true},
        {Exchange::OKX, 100.0, 100.0, 0.10, 7.0, 0.2, 0.2, true},
        {Exchange::COINBASE, 100.0, 100.3, 1.00, 5.0, 0.2, 0.2, true},
    } };

    RoutingDecision d = sor.route(Side::BID, 0.25, venues);

    ASSERT_EQ(d.child_count, 2u);
    EXPECT_EQ(d.children[0].exchange, Exchange::OKX);
    EXPECT_NEAR(d.children[0].quantity, 0.10, 1e-12);
    EXPECT_EQ(d.children[1].exchange, Exchange::KRAKEN);
    EXPECT_NEAR(d.children[1].quantity, 0.15, 1e-12);
}

TEST(SmartOrderRouterTest, SkipsUnhealthyOrEmptyVenues) {
    SmartOrderRouter sor;
    std::array<VenueQuote, SmartOrderRouter::MAX_VENUES> venues{ {
        {Exchange::BINANCE, 100.0, 100.1, 0.0, 5.0, 0.1, 0.1, true},
        {Exchange::KRAKEN, 100.0, 100.2, 1.0, 4.0, 0.1, 0.1, false},
        {Exchange::OKX, 100.0, 100.0, 1.0, 4.0, 0.1, 0.1, true},
        {Exchange::COINBASE, 100.0, 100.3, 1.0, 2.0, 0.1, 0.1, true},
    } };

    RoutingDecision d = sor.route(Side::BID, 0.5, venues);

    ASSERT_EQ(d.child_count, 1u);
    EXPECT_EQ(d.children[0].exchange, Exchange::OKX);
    EXPECT_NEAR(d.children[0].quantity, 0.5, 1e-12);
}

}  // namespace trading
