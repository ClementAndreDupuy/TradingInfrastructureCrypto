#include "core/execution/common/orders/child_order_scheduler.hpp"

#include <gtest/gtest.h>

#include <array>

namespace trading {
namespace {

std::array<VenueQuote, SmartOrderRouter::MAX_VENUES> make_venues() {
    return {{
        {Exchange::BINANCE, 100.0, 100.1, 0.40, 2.0, 0.4, 0.2, 0.55, 2.50, 1.2, true},
        {Exchange::KRAKEN, 100.0, 100.1, 0.50, 2.2, 0.2, 0.2, 0.88, 0.10, 0.2, true},
        {Exchange::OKX, 100.0, 100.1, 0.45, 2.1, 0.1, 0.2, 0.82, 0.20, 0.3, true},
        {Exchange::COINBASE, 100.0, 100.1, 0.35, 2.5, 0.3, 0.2, 0.72, 0.30, 0.4, true},
    }};
}

ParentExecutionPlan make_plan(Side side, double qty, ShadowUrgency urgency) {
    ParentExecutionPlan plan;
    plan.side = side;
    plan.total_qty = qty;
    plan.remaining_qty = qty;
    plan.urgency = urgency;
    plan.allow_passive = urgency != ShadowUrgency::AGGRESSIVE;
    plan.allow_aggressive = true;
    plan.state = ParentPlanState::WORKING;
    return plan;
}

TEST(ChildOrderSchedulerTest, LongHorizonPassiveIntentPostsJoinOrder) {
    ChildOrderScheduler scheduler;
    const auto plan = make_plan(Side::BID, 0.25, ShadowUrgency::PASSIVE);

    const SchedulerDecision decision = scheduler.schedule(plan, 500, 0, make_venues());

    ASSERT_EQ(decision.routing.child_count, 1u);
    EXPECT_EQ(decision.style, ChildExecutionStyle::PASSIVE_JOIN);
    EXPECT_EQ(decision.routing.children[0].exchange, Exchange::KRAKEN);
    EXPECT_EQ(decision.routing.children[0].tif, TimeInForce::GTX);
    EXPECT_DOUBLE_EQ(decision.routing.children[0].limit_price, 100.0);
}

TEST(ChildOrderSchedulerTest, ShortHorizonAggressivePlanSweepsMultipleVenues) {
    ChildOrderScheduler scheduler;
    const auto plan = make_plan(Side::BID, 0.80, ShadowUrgency::AGGRESSIVE);

    const SchedulerDecision decision = scheduler.schedule(plan, 2, 0, make_venues());

    ASSERT_EQ(decision.style, ChildExecutionStyle::SWEEP);
    ASSERT_EQ(decision.routing.child_count, 2u);
    EXPECT_EQ(decision.routing.children[0].exchange, Exchange::KRAKEN);
    EXPECT_EQ(decision.routing.children[0].tif, TimeInForce::IOC);
    EXPECT_EQ(decision.routing.children[1].exchange, Exchange::OKX);
    EXPECT_NEAR(decision.routing.children[0].quantity + decision.routing.children[1].quantity, 0.80,
                1e-12);
}

TEST(ChildOrderSchedulerTest, HighToxicityAndLowFillVenueIsDeprioritized) {
    ChildOrderScheduler scheduler;
    auto venues = make_venues();
    venues[0].best_ask = 100.0;
    venues[0].fill_probability = 0.10;
    venues[0].toxicity_bps = 4.5;
    venues[1].best_ask = 100.2;

    const auto plan = make_plan(Side::BID, 0.20, ShadowUrgency::BALANCED);
    const SchedulerDecision decision = scheduler.schedule(plan, 8, 0, venues);

    ASSERT_EQ(decision.routing.child_count, 1u);
    EXPECT_EQ(decision.style, ChildExecutionStyle::IOC);
    EXPECT_EQ(decision.routing.children[0].exchange, Exchange::KRAKEN);
}

} 
}
