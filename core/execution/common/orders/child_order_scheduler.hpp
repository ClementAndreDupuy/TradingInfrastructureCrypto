#pragma once

#include "parent_order_manager.hpp"
#include "../../router/smart_order_router.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace trading {
    enum class ChildExecutionStyle : uint8_t {
        PASSIVE_JOIN = 0,
        PASSIVE_IMPROVE,
        IOC,
        SWEEP,
    };

    struct SchedulerDecision {
        RoutingDecision routing{};
        ChildExecutionStyle style = ChildExecutionStyle::IOC;
        double expected_shortfall_bps = 0.0;
    };

    class ChildOrderScheduler {
    public:
        struct Config {
            int64_t short_horizon_ticks = 12;
            int64_t sweep_horizon_ticks = 3;
            int64_t long_horizon_ticks = 80;
            double passive_join_inventory_age_bps = 0.4;
            double passive_improve_inventory_age_bps = 0.25;
            double aggressive_inventory_age_bps = 0.1;
            double passive_improve_fraction = 0.20;
            double price_cross_epsilon = 1e-6;
            SmartOrderRouterConfig sor;
        };

        ChildOrderScheduler() = default;

        explicit ChildOrderScheduler(Config cfg) : cfg_(cfg) {
        }

        [[nodiscard]] SchedulerDecision schedule(
            const ParentExecutionPlan &plan,
            int64_t horizon_ticks,
            int64_t inventory_age_ms,
            const std::array<VenueQuote, SmartOrderRouter::MAX_VENUES> &venues) const noexcept {
            SchedulerDecision out;
            if (!plan.active() || plan.remaining_qty <= 0.0) {
                return out;
            }

            const auto style = choose_style(plan, horizon_ticks);
            out.style = style;

            const double best_px = best_reference_price(plan.side, style, venues);
            if (best_px <= 0.0) {
                return out;
            }

            const RoutingRegime regime = SmartOrderRouter::infer_regime(venues, cfg_.sor);
            std::array<bool, SmartOrderRouter::MAX_VENUES> used{};
            double remaining = plan.remaining_qty;
            double total_weighted_shortfall = 0.0;
            const size_t venue_limit = max_venue_count(style);

            while (remaining > k_qty_epsilon && out.routing.child_count < out.routing.children.size() &&
                   out.routing.child_count < venue_limit) {
                const int best_idx = select_best_venue(used, plan.side, style, regime, best_px,
                                                       inventory_age_ms, venues);
                if (best_idx < 0) {
                    break;
                }

                const auto &venue = venues[best_idx];
                const double clip = std::min(remaining, venue.depth_qty);
                if (clip <= k_qty_epsilon) {
                    used[best_idx] = true;
                    continue;
                }

                ChildOrder child;
                child.exchange = venue.exchange;
                child.quantity = clip;
                child.limit_price = limit_price_for_style(venue, plan.side, style);
                child.tif = tif_for_style(style);
                out.routing.children[out.routing.child_count++] = child;

                total_weighted_shortfall +=
                    clip * venue_expected_shortfall(venue, plan.side, style, regime, best_px,
                                                    inventory_age_ms);
                remaining -= clip;
                used[best_idx] = true;

                if (style != ChildExecutionStyle::SWEEP) {
                    break;
                }
            }

            if (out.routing.child_count == 0) {
                return out;
            }

            const double routed_qty = plan.remaining_qty - remaining;
            if (routed_qty > k_qty_epsilon) {
                out.expected_shortfall_bps = total_weighted_shortfall / routed_qty;
            }
            return out;
        }

    private:
        static constexpr double k_qty_epsilon = 1e-9;

        Config cfg_{};

        [[nodiscard]] static bool lower_price_is_better(Side side,
                                                        ChildExecutionStyle style) noexcept {
            const bool passive_style = style == ChildExecutionStyle::PASSIVE_JOIN ||
                                       style == ChildExecutionStyle::PASSIVE_IMPROVE;
            if (passive_style) {
                return side == Side::ASK;
            }
            return side == Side::BID;
        }

        [[nodiscard]] ChildExecutionStyle choose_style(const ParentExecutionPlan &plan,
                                                       int64_t horizon_ticks) const noexcept {
            if (plan.urgency == ShadowUrgency::AGGRESSIVE) {
                return horizon_ticks <= cfg_.sweep_horizon_ticks ? ChildExecutionStyle::SWEEP
                                                                 : ChildExecutionStyle::IOC;
            }
            if (plan.urgency == ShadowUrgency::PASSIVE) {
                return horizon_ticks >= cfg_.long_horizon_ticks ? ChildExecutionStyle::PASSIVE_JOIN
                                                                : ChildExecutionStyle::PASSIVE_IMPROVE;
            }
            if (horizon_ticks <= cfg_.sweep_horizon_ticks) {
                return ChildExecutionStyle::SWEEP;
            }
            if (horizon_ticks <= cfg_.short_horizon_ticks) {
                return ChildExecutionStyle::IOC;
            }
            return ChildExecutionStyle::PASSIVE_IMPROVE;
        }

        [[nodiscard]] static size_t max_venue_count(ChildExecutionStyle style) noexcept {
            return style == ChildExecutionStyle::SWEEP ? SmartOrderRouter::MAX_VENUES : 1u;
        }

        [[nodiscard]] double inventory_age_penalty_bps(ChildExecutionStyle style,
                                                       int64_t inventory_age_ms) const noexcept {
            const double age_scale = std::clamp(static_cast<double>(std::max<int64_t>(0, inventory_age_ms)) /
                                                    1000.0,
                                                0.0, 10.0);
            switch (style) {
                case ChildExecutionStyle::PASSIVE_JOIN:
                    return age_scale * cfg_.passive_join_inventory_age_bps;
                case ChildExecutionStyle::PASSIVE_IMPROVE:
                    return age_scale * cfg_.passive_improve_inventory_age_bps;
                case ChildExecutionStyle::IOC:
                case ChildExecutionStyle::SWEEP:
                    return age_scale * cfg_.aggressive_inventory_age_bps;
            }
            return 0.0;
        }

        [[nodiscard]] static double quote_price_for_style(const VenueQuote &venue, Side side,
                                                          ChildExecutionStyle style) noexcept {
            if (style == ChildExecutionStyle::PASSIVE_JOIN ||
                style == ChildExecutionStyle::PASSIVE_IMPROVE) {
                return side == Side::BID ? venue.best_bid : venue.best_ask;
            }
            return side == Side::BID ? venue.best_ask : venue.best_bid;
        }

        [[nodiscard]] double best_reference_price(
            Side side,
            ChildExecutionStyle style,
            const std::array<VenueQuote, SmartOrderRouter::MAX_VENUES> &venues) const noexcept {
            double best_px = 0.0;
            bool found = false;
            for (const auto &venue: venues) {
                if (!venue.healthy || venue.depth_qty <= 0.0) {
                    continue;
                }
                const double px = quote_price_for_style(venue, side, style);
                if (px <= 0.0) {
                    continue;
                }
                if (!found || (lower_price_is_better(side, style) ? px < best_px : px > best_px)) {
                    best_px = px;
                    found = true;
                }
            }
            return found ? best_px : 0.0;
        }

        [[nodiscard]] int select_best_venue(
            const std::array<bool, SmartOrderRouter::MAX_VENUES> &used,
            Side side,
            ChildExecutionStyle style,
            const RoutingRegime &regime,
            double best_px,
            int64_t inventory_age_ms,
            const std::array<VenueQuote, SmartOrderRouter::MAX_VENUES> &venues) const noexcept {
            int best_idx = -1;
            double best_score = 1e18;
            for (size_t i = 0; i < venues.size(); ++i) {
                if (used[i]) {
                    continue;
                }
                const auto &venue = venues[i];
                if (!venue.healthy || venue.depth_qty <= 0.0) {
                    continue;
                }
                const double score =
                    venue_expected_shortfall(venue, side, style, regime, best_px, inventory_age_ms);
                if (score < best_score) {
                    best_score = score;
                    best_idx = static_cast<int>(i);
                }
            }
            return best_idx;
        }

        [[nodiscard]] double venue_expected_shortfall(const VenueQuote &venue, Side side,
                                                      ChildExecutionStyle style,
                                                      const RoutingRegime &regime, double best_px,
                                                      int64_t inventory_age_ms) const noexcept {
            const double score = SmartOrderRouter::expected_shortfall_bps(
                venue, side, regime, best_px, inventory_age_penalty_bps(style, inventory_age_ms),
                cfg_.sor.min_fill_probability);
            if (style == ChildExecutionStyle::PASSIVE_JOIN) {
                return score - (0.5 * venue.taker_fee_bps) + venue.passive_markout_bps;
            }
            if (style == ChildExecutionStyle::PASSIVE_IMPROVE) {
                return score - (0.25 * venue.taker_fee_bps) + 0.10 * venue.queue_ahead_qty +
                       venue.passive_markout_bps;
            }
            if (style == ChildExecutionStyle::SWEEP) {
                return score - 0.20 * venue.latency_penalty_bps + venue.taker_markout_bps;
            }
            return score + venue.taker_markout_bps;
        }

        [[nodiscard]] TimeInForce tif_for_style(ChildExecutionStyle style) const noexcept {
            switch (style) {
                case ChildExecutionStyle::PASSIVE_JOIN:
                case ChildExecutionStyle::PASSIVE_IMPROVE:
                    return TimeInForce::GTX;
                case ChildExecutionStyle::IOC:
                case ChildExecutionStyle::SWEEP:
                    return TimeInForce::IOC;
            }
            return TimeInForce::IOC;
        }

        [[nodiscard]] double limit_price_for_style(const VenueQuote &venue, Side side,
                                                   ChildExecutionStyle style) const noexcept {
            if (style == ChildExecutionStyle::PASSIVE_JOIN) {
                return side == Side::BID ? venue.best_bid : venue.best_ask;
            }
            if (style == ChildExecutionStyle::PASSIVE_IMPROVE) {
                const double same_side = side == Side::BID ? venue.best_bid : venue.best_ask;
                const double opposite_side = side == Side::BID ? venue.best_ask : venue.best_bid;
                if (same_side <= 0.0 || opposite_side <= 0.0 || opposite_side <= same_side) {
                    return same_side;
                }
                const double spread = opposite_side - same_side;
                const double improvement = std::max(
                    0.0, std::min(spread * cfg_.passive_improve_fraction,
                                  spread - cfg_.price_cross_epsilon));
                return side == Side::BID ? same_side + improvement : same_side - improvement;
            }
            return side == Side::BID ? venue.best_ask : venue.best_bid;
        }
    };
}
