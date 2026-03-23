#include "core/execution/common/venue_quality_model.hpp"
#include "core/execution/common/child_order_scheduler.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>

namespace trading {
namespace {

std::array<VenueQuote, SmartOrderRouter::MAX_VENUES> make_scheduler_venues() {
    return {{
        {Exchange::BINANCE, 100.0, 100.1, 0.50, 2.0, 0.3, 0.2, 0.75, 0.30, 0.4, true},
        {Exchange::KRAKEN, 100.0, 100.1, 0.50, 2.0, 0.2, 0.2, 0.75, 0.20, 0.4, true},
        {Exchange::OKX, 100.0, 100.1, 0.50, 2.0, 0.2, 0.2, 0.75, 0.20, 0.4, true},
        {Exchange::COINBASE, 100.0, 100.1, 0.50, 2.0, 0.2, 0.2, 0.75, 0.20, 0.4, true},
    }};
}

ParentExecutionPlan make_plan() {
    ParentExecutionPlan plan;
    plan.side = Side::BID;
    plan.total_qty = 0.25;
    plan.remaining_qty = 0.25;
    plan.urgency = ShadowUrgency::PASSIVE;
    plan.allow_passive = true;
    plan.allow_aggressive = true;
    plan.state = ParentPlanState::WORKING;
    return plan;
}

TEST(VenueQualityModelTest, AdaptationIsBoundedUnderNoisyInputs) {
    VenueQualityModel model;

    for (int i = 0; i < 64; ++i) {
        model.observe_fill_probability(Exchange::OKX, i % 2 == 0 ? 0.0 : 1.0);
        model.observe_markout(Exchange::OKX, true, i % 2 == 0 ? -4.0 : 3.5);
        model.observe_reject(Exchange::OKX, i % 3 == 0);
    }

    const VenueQualitySnapshot snap = model.snapshot(Exchange::OKX);
    EXPECT_GE(snap.composite_fill_probability, 0.10);
    EXPECT_LE(snap.composite_fill_probability, 0.98);
    EXPECT_LT(snap.passive_markout_bps, 1.0);
    EXPECT_GT(snap.passive_markout_bps, -1.5);
    EXPECT_LT(snap.reject_rate, 0.6);
}

TEST(VenueQualityModelTest, SchedulerUsesAdaptiveVenuePenalty) {
    VenueQualityModel model;
    ChildOrderScheduler scheduler;
    auto venues = make_scheduler_venues();

    for (int i = 0; i < 12; ++i) {
        model.observe_fill_probability(Exchange::BINANCE, 0.20);
        model.observe_markout(Exchange::BINANCE, true, -2.5);
        model.observe_reject(Exchange::BINANCE, true);
        model.observe_cancel_latency(Exchange::BINANCE, std::chrono::microseconds(24000));
        model.observe_health(Exchange::BINANCE, false);

        model.observe_fill_probability(Exchange::KRAKEN, 0.95);
        model.observe_markout(Exchange::KRAKEN, true, 0.6);
        model.observe_reject(Exchange::KRAKEN, false);
        model.observe_cancel_latency(Exchange::KRAKEN, std::chrono::microseconds(1000));
        model.observe_health(Exchange::KRAKEN, true);
    }

    model.apply(venues);
    const SchedulerDecision decision = scheduler.schedule(make_plan(), 300, 0, venues);

    ASSERT_EQ(decision.routing.child_count, 1u);
    EXPECT_EQ(decision.style, ChildExecutionStyle::PASSIVE_JOIN);
    EXPECT_EQ(decision.routing.children[0].exchange, Exchange::KRAKEN);
    EXPECT_GT(model.snapshot(Exchange::BINANCE).composite_penalty_bps,
              model.snapshot(Exchange::KRAKEN).composite_penalty_bps);
}

TEST(VenueQualityModelTest, RoutingRemainsStableUnderShortTermNoise) {
    VenueQualityModel model;
    ChildOrderScheduler scheduler;
    auto venues = make_scheduler_venues();
    const ParentExecutionPlan plan = make_plan();

    size_t kraken_wins = 0;
    for (int i = 0; i < 32; ++i) {
        model.observe_fill_probability(Exchange::KRAKEN, i % 2 == 0 ? 0.88 : 0.82);
        model.observe_markout(Exchange::KRAKEN, true, i % 2 == 0 ? 0.8 : 0.2);
        model.observe_reject(Exchange::KRAKEN, false);
        model.observe_cancel_latency(Exchange::KRAKEN, std::chrono::microseconds(1200 + (i % 3) * 200));
        model.observe_health(Exchange::KRAKEN, true);

        model.observe_fill_probability(Exchange::BINANCE, i % 2 == 0 ? 0.55 : 0.45);
        model.observe_markout(Exchange::BINANCE, true, i % 2 == 0 ? -0.9 : -0.2);
        model.observe_reject(Exchange::BINANCE, i % 5 == 0);
        model.observe_cancel_latency(Exchange::BINANCE, std::chrono::microseconds(4000 + (i % 4) * 500));
        model.observe_health(Exchange::BINANCE, true);

        auto iteration_venues = venues;
        model.apply(iteration_venues);
        const SchedulerDecision decision = scheduler.schedule(plan, 300, 0, iteration_venues);
        ASSERT_EQ(decision.routing.child_count, 1u);
        if (decision.routing.children[0].exchange == Exchange::KRAKEN) {
            ++kraken_wins;
        }
    }

    EXPECT_GE(kraken_wins, 28u);
    EXPECT_GT(model.snapshot(Exchange::KRAKEN).composite_fill_probability,
              model.snapshot(Exchange::BINANCE).composite_fill_probability);
}

}
}
