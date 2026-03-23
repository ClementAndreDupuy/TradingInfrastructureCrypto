#pragma once

#include "../../../shadow/shadow_engine.hpp"
#include "../../../common/types.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace trading {
    enum class ParentPlanState : uint8_t {
        IDLE = 0,
        WORKING,
        COMPLETED,
        CANCELED,
        REPLACED,
        EXPIRED,
        REJECTED,
    };

    enum class ParentPlanAction : uint8_t {
        NONE = 0,
        CREATED,
        UPDATED,
        REPLACED,
        EXPIRED,
        ESCALATED,
        COMPLETED,
        CANCELED,
        REJECTED,
    };

    struct ParentExecutionPlan {
        uint64_t plan_id = 0;
        Side side = Side::BID;
        double total_qty = 0.0;
        double filled_qty = 0.0;
        double remaining_qty = 0.0;
        ShadowUrgency urgency = ShadowUrgency::BALANCED;
        bool allow_passive = true;
        bool allow_aggressive = true;
        std::chrono::steady_clock::time_point created_at{};
        std::chrono::steady_clock::time_point deadline{};
        ParentPlanState state = ParentPlanState::IDLE;
        uint32_t revision = 0;

        [[nodiscard]] bool active() const noexcept {
            return state == ParentPlanState::WORKING;
        }
    };

    struct ParentPlanUpdateResult {
        ParentPlanAction action = ParentPlanAction::NONE;
        ParentExecutionPlan plan{};
        bool should_cancel_working_children = false;
        bool deadline_passed = false;
    };

    class ParentOrderManager {
    public:
        struct Config {
            double replace_qty_threshold = 0.05;
            std::chrono::milliseconds default_deadline{1500};
            std::chrono::milliseconds min_deadline_extension{250};
        };

        ParentOrderManager() = default;

        explicit ParentOrderManager(Config cfg) : cfg_(cfg) {
        }

        [[nodiscard]] ParentPlanUpdateResult update_target(
            double position_delta,
            ShadowUrgency urgency,
            std::chrono::steady_clock::time_point now,
            std::chrono::milliseconds deadline = std::chrono::milliseconds::zero()) noexcept {
            ParentPlanUpdateResult result;
            maybe_expire(now, result);

            const double requested_qty = std::abs(position_delta);
            if (requested_qty <= k_qty_epsilon) {
                if (plan_.active()) {
                    plan_.state = ParentPlanState::CANCELED;
                    ++plan_.revision;
                    result.action = ParentPlanAction::CANCELED;
                    result.should_cancel_working_children = true;
                    result.plan = plan_;
                } else if (result.action == ParentPlanAction::NONE) {
                    result.plan = plan_;
                }
                return result;
            }

            const Side requested_side = position_delta > 0.0 ? Side::BID : Side::ASK;
            const auto effective_deadline = now + (deadline == std::chrono::milliseconds::zero()
                                                      ? cfg_.default_deadline
                                                      : deadline);

            if (!plan_.active()) {
                plan_ = build_plan(requested_side, requested_qty, urgency, now, effective_deadline,
                                   next_plan_id_++);
                result.action = ParentPlanAction::CREATED;
                result.plan = plan_;
                return result;
            }

            if (plan_.side != requested_side ||
                std::abs(plan_.total_qty - requested_qty) >= cfg_.replace_qty_threshold) {
                plan_.state = ParentPlanState::REPLACED;
                ++plan_.revision;
                result.action = ParentPlanAction::REPLACED;
                result.should_cancel_working_children = true;
                plan_ = build_plan(requested_side, requested_qty, urgency, now, effective_deadline,
                                   next_plan_id_++);
                result.plan = plan_;
                return result;
            }

            const bool urgency_changed = plan_.urgency != urgency;
            const bool deadline_extended = effective_deadline > plan_.deadline + cfg_.min_deadline_extension;
            const double capped_remaining_qty = std::min(plan_.remaining_qty, requested_qty);
            const bool quantity_changed =
                    std::abs(plan_.total_qty - requested_qty) >= k_qty_epsilon ||
                    std::abs(plan_.remaining_qty - capped_remaining_qty) >= k_qty_epsilon;
            plan_.total_qty = std::max(plan_.filled_qty, requested_qty);
            plan_.remaining_qty = std::max(0.0, capped_remaining_qty);
            plan_.urgency = urgency;
            plan_.deadline = std::max(plan_.deadline, effective_deadline);
            plan_.allow_passive = urgency != ShadowUrgency::AGGRESSIVE;
            plan_.allow_aggressive = true;
            ++plan_.revision;
            result.action = urgency_changed || deadline_extended || quantity_changed
                                    ? ParentPlanAction::UPDATED
                                    : ParentPlanAction::NONE;
            result.plan = plan_;
            return result;
        }

        void on_child_fill(double fill_qty) noexcept {
            if (!plan_.active() || fill_qty <= 0.0)
                return;
            plan_.filled_qty = std::min(plan_.total_qty, plan_.filled_qty + fill_qty);
            plan_.remaining_qty = std::max(0.0, plan_.total_qty - plan_.filled_qty);
            if (plan_.remaining_qty <= k_qty_epsilon) {
                plan_.remaining_qty = 0.0;
                plan_.state = ParentPlanState::COMPLETED;
            }
        }

        void on_child_cancel(double canceled_qty) noexcept {
            if (!plan_.active() || canceled_qty <= 0.0)
                return;
            plan_.remaining_qty = std::max(0.0, plan_.remaining_qty - canceled_qty);
            plan_.total_qty = std::max(plan_.filled_qty, plan_.filled_qty + plan_.remaining_qty);
            if (plan_.remaining_qty <= k_qty_epsilon) {
                plan_.remaining_qty = 0.0;
                plan_.state = plan_.filled_qty > 0.0 ? ParentPlanState::COMPLETED
                                                     : ParentPlanState::CANCELED;
            }
        }

        void on_child_reject() noexcept {
            if (!plan_.active())
                return;
            plan_.state = ParentPlanState::REJECTED;
            plan_.remaining_qty = std::max(0.0, plan_.total_qty - plan_.filled_qty);
        }

        [[nodiscard]] ParentPlanUpdateResult poll(std::chrono::steady_clock::time_point now) noexcept {
            ParentPlanUpdateResult result;
            maybe_expire(now, result);
            if (result.action == ParentPlanAction::NONE)
                result.plan = plan_;
            return result;
        }

        [[nodiscard]] ParentExecutionPlan snapshot() const noexcept {
            return plan_;
        }

        [[nodiscard]] bool has_active_plan() const noexcept {
            return plan_.active();
        }

    private:
        static constexpr double k_qty_epsilon = 1e-9;

        Config cfg_{};
        ParentExecutionPlan plan_{};
        uint64_t next_plan_id_ = 1;

        [[nodiscard]] static ParentExecutionPlan build_plan(
            Side side,
            double qty,
            ShadowUrgency urgency,
            std::chrono::steady_clock::time_point now,
            std::chrono::steady_clock::time_point deadline,
            uint64_t plan_id) noexcept {
            ParentExecutionPlan plan;
            plan.plan_id = plan_id;
            plan.side = side;
            plan.total_qty = qty;
            plan.remaining_qty = qty;
            plan.urgency = urgency;
            plan.allow_passive = urgency != ShadowUrgency::AGGRESSIVE;
            plan.allow_aggressive = true;
            plan.created_at = now;
            plan.deadline = deadline;
            plan.state = ParentPlanState::WORKING;
            plan.revision = 1;
            return plan;
        }

        void maybe_expire(std::chrono::steady_clock::time_point now,
                          ParentPlanUpdateResult &result) noexcept {
            if (!plan_.active() || now < plan_.deadline)
                return;
            plan_.state = ParentPlanState::EXPIRED;
            plan_.urgency = ShadowUrgency::AGGRESSIVE;
            plan_.allow_passive = false;
            ++plan_.revision;
            result.action = ParentPlanAction::EXPIRED;
            result.plan = plan_;
            result.should_cancel_working_children = plan_.remaining_qty > k_qty_epsilon;
            result.deadline_passed = true;
        }
    };
}
