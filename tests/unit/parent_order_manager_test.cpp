#include "core/execution/common/parent_order_manager.hpp"

#include <gtest/gtest.h>

namespace trading {
namespace {

using Clock = std::chrono::steady_clock;

TEST(ParentOrderManagerTest, CreatesOneWorkingPlanFromTargetDelta) {
    ParentOrderManager manager;
    const auto now = Clock::now();

    const auto result = manager.update_target(0.40, ShadowUrgency::PASSIVE, now);

    EXPECT_EQ(result.action, ParentPlanAction::CREATED);
    EXPECT_EQ(result.plan.plan_id, 1u);
    EXPECT_EQ(result.plan.side, Side::BID);
    EXPECT_DOUBLE_EQ(result.plan.total_qty, 0.40);
    EXPECT_DOUBLE_EQ(result.plan.remaining_qty, 0.40);
    EXPECT_EQ(result.plan.urgency, ShadowUrgency::PASSIVE);
    EXPECT_TRUE(result.plan.allow_passive);
    EXPECT_TRUE(manager.has_active_plan());
}

TEST(ParentOrderManagerTest, FillProgressUpdatesRemainingQuantity) {
    ParentOrderManager manager;
    const auto now = Clock::now();
    manager.update_target(0.40, ShadowUrgency::BALANCED, now);

    manager.on_child_fill(0.25);
    auto snapshot = manager.snapshot();
    EXPECT_DOUBLE_EQ(snapshot.filled_qty, 0.25);
    EXPECT_DOUBLE_EQ(snapshot.remaining_qty, 0.15);
    EXPECT_EQ(snapshot.state, ParentPlanState::WORKING);

    manager.on_child_fill(0.15);
    snapshot = manager.snapshot();
    EXPECT_DOUBLE_EQ(snapshot.filled_qty, 0.40);
    EXPECT_DOUBLE_EQ(snapshot.remaining_qty, 0.0);
    EXPECT_EQ(snapshot.state, ParentPlanState::COMPLETED);
}

TEST(ParentOrderManagerTest, MeaningfulTargetChangeReplacesPlan) {
    ParentOrderManager manager;
    const auto now = Clock::now();
    const auto first = manager.update_target(0.30, ShadowUrgency::PASSIVE, now);
    const auto second = manager.update_target(-0.20, ShadowUrgency::AGGRESSIVE,
                                              now + std::chrono::milliseconds(5));

    EXPECT_EQ(first.plan.plan_id, 1u);
    EXPECT_EQ(second.action, ParentPlanAction::REPLACED);
    EXPECT_TRUE(second.should_cancel_working_children);
    EXPECT_EQ(second.plan.plan_id, 2u);
    EXPECT_EQ(second.plan.side, Side::ASK);
    EXPECT_DOUBLE_EQ(second.plan.total_qty, 0.20);
    EXPECT_EQ(second.plan.urgency, ShadowUrgency::AGGRESSIVE);
    EXPECT_FALSE(second.plan.allow_passive);
}

TEST(ParentOrderManagerTest, SmallUpdateKeepsSinglePlanObservable) {
    ParentOrderManager manager;
    const auto now = Clock::now();
    const auto first = manager.update_target(0.30, ShadowUrgency::BALANCED, now);
    const auto second = manager.update_target(0.32, ShadowUrgency::AGGRESSIVE,
                                              now + std::chrono::milliseconds(10));

    EXPECT_EQ(second.plan.plan_id, first.plan.plan_id);
    EXPECT_EQ(second.action, ParentPlanAction::UPDATED);
    EXPECT_EQ(second.plan.side, Side::BID);
    EXPECT_DOUBLE_EQ(second.plan.total_qty, 0.30);
    EXPECT_EQ(second.plan.urgency, ShadowUrgency::AGGRESSIVE);
    EXPECT_FALSE(second.plan.allow_passive);
    EXPECT_TRUE(second.plan.allow_aggressive);
}

TEST(ParentOrderManagerTest, DeadlineExpiryMarksPlanExpired) {
    ParentOrderManager manager;
    const auto now = Clock::now();
    manager.update_target(0.35, ShadowUrgency::PASSIVE, now, std::chrono::milliseconds(20));

    const auto poll = manager.poll(now + std::chrono::milliseconds(25));

    EXPECT_EQ(poll.action, ParentPlanAction::EXPIRED);
    EXPECT_TRUE(poll.deadline_passed);
    EXPECT_TRUE(poll.should_cancel_working_children);
    EXPECT_EQ(poll.plan.state, ParentPlanState::EXPIRED);
    EXPECT_EQ(poll.plan.urgency, ShadowUrgency::AGGRESSIVE);
    EXPECT_FALSE(poll.plan.allow_passive);
}

TEST(ParentOrderManagerTest, ZeroDeltaCancelsWorkingPlan) {
    ParentOrderManager manager;
    const auto now = Clock::now();
    manager.update_target(0.25, ShadowUrgency::BALANCED, now);

    const auto result = manager.update_target(0.0, ShadowUrgency::BALANCED,
                                              now + std::chrono::milliseconds(1));

    EXPECT_EQ(result.action, ParentPlanAction::CANCELED);
    EXPECT_TRUE(result.should_cancel_working_children);
    EXPECT_EQ(result.plan.state, ParentPlanState::CANCELED);
    EXPECT_FALSE(manager.has_active_plan());
}

} 
}
