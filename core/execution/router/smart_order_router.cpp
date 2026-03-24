#include "smart_order_router.hpp"

#include <cmath>

namespace trading {
    namespace {
        constexpr double k_infinite_price = 1e18;
        constexpr double k_missing_best_price = 1e17;
        constexpr double k_min_remaining_qty = 1e-12;

        auto quote_price_for_side(const VenueQuote &venue_quote, Side side) noexcept -> double {
            return (side == Side::BID) ? venue_quote.best_ask : venue_quote.best_bid;
        }

        auto lower_price_is_better(Side side) noexcept -> bool { return side == Side::BID; }

        auto find_best_price(Side side,
                             const std::array<VenueQuote, SmartOrderRouter::MAX_VENUES> &venues) -> double {
            double best_px = 0.0;
            bool found = false;
            for (const auto &venue_quote: venues) {
                if (!venue_quote.healthy || venue_quote.depth_qty <= 0.0) {
                    continue;
                }
                const double quote_price = quote_price_for_side(venue_quote, side);
                if (quote_price <= 0.0) {
                    continue;
                }
                if (!found || (lower_price_is_better(side) ? quote_price < best_px
                                                           : quote_price > best_px)) {
                    best_px = quote_price;
                    found = true;
                }
            }
            return found ? best_px : k_infinite_price;
        }

        auto find_best_venue_index(const std::array<VenueQuote, SmartOrderRouter::MAX_VENUES> &venues,
                                   const std::array<bool, SmartOrderRouter::MAX_VENUES> &used, Side side,
                                   const RoutingRegime &regime, double best_px,
                                   double min_fill_probability) noexcept -> int {
            int best_idx = -1;
            double best_score = k_infinite_price;

            for (size_t i = 0; i < venues.size(); ++i) {
                if (used[i]) {
                    continue;
                }
                const auto &venue_quote = venues[i];
                if (!venue_quote.healthy || venue_quote.depth_qty <= 0.0) {
                    continue;
                }

                const double score = SmartOrderRouter::expected_shortfall_bps(
                    venue_quote, side, regime, best_px, 0.0, min_fill_probability);
                if (score < best_score) {
                    best_score = score;
                    best_idx = static_cast<int>(i);
                }
            }

            return best_idx;
        }

        void append_child_order(RoutingDecision &out, Side side, const VenueQuote &winner,
                                double &remaining, std::array<bool, SmartOrderRouter::MAX_VENUES> &used,
                                int best_idx) noexcept {
            const double clip = (remaining < winner.depth_qty) ? remaining : winner.depth_qty;

            ChildOrder child;
            child.exchange = winner.exchange;
            child.quantity = clip;
            child.limit_price = quote_price_for_side(winner, side);
            child.tif = TimeInForce::IOC;
            out.children[out.child_count++] = child;

            remaining -= clip;
            used[best_idx] = true;
        }
    }

    auto SmartOrderRouter::effective_price_bps(const VenueQuote &venue_quote,
                                               Side side) noexcept -> double {
        const double quote_price = quote_price_for_side(venue_quote, side);
        if (quote_price <= 0.0) {
            return 1e12;
        }

        return venue_quote.taker_fee_bps + venue_quote.latency_penalty_bps +
               venue_quote.risk_penalty_bps;
    }

    auto SmartOrderRouter::infer_regime(const std::array<VenueQuote, MAX_VENUES> &venues,
                                        const SmartOrderRouterConfig &sor_cfg) noexcept
        -> RoutingRegime {
        double healthy_count = 0.0;
        double total_toxicity = 0.0;
        double total_fill = 0.0;

        for (const auto &venue_quote: venues) {
            if (!venue_quote.healthy) {
                continue;
            }
            healthy_count += 1.0;
            total_toxicity += venue_quote.toxicity_bps;
            total_fill += venue_quote.adaptive_fill_probability >= 0.0
                              ? venue_quote.adaptive_fill_probability
                              : venue_quote.fill_probability;
        }

        RoutingRegime regime;
        if (healthy_count <= 0.0) {
            return regime;
        }

        const double avg_toxicity = total_toxicity / healthy_count;
        const double avg_fill = total_fill / healthy_count;

        if (avg_toxicity >= sor_cfg.high_toxicity_threshold) {
            regime.fill_weight_bps = sor_cfg.high_toxicity_fill_weight;
            regime.queue_weight_bps = sor_cfg.high_toxicity_queue_weight;
            regime.toxicity_weight = sor_cfg.high_toxicity_tox_weight;
            return regime;
        }

        if (avg_fill < sor_cfg.low_fill_threshold) {
            regime.fill_weight_bps = sor_cfg.low_fill_fill_weight;
            regime.queue_weight_bps = sor_cfg.low_fill_queue_weight;
            regime.toxicity_weight = sor_cfg.low_fill_tox_weight;
            return regime;
        }

        if (avg_fill > sor_cfg.high_fill_threshold) {
            regime.fill_weight_bps = sor_cfg.high_fill_fill_weight;
            regime.queue_weight_bps = sor_cfg.high_fill_queue_weight;
            regime.toxicity_weight = sor_cfg.high_fill_tox_weight;
        }

        return regime;
    }

    auto SmartOrderRouter::score_venue_bps(const VenueQuote &venue_quote, Side side,
                                           const RoutingRegime &regime,
                                           double min_fill_probability) noexcept -> double {
        const double base_cost = effective_price_bps(venue_quote, side);
        const double effective_fill_probability =
            venue_quote.adaptive_fill_probability >= 0.0 ? venue_quote.adaptive_fill_probability
                                                         : venue_quote.fill_probability;
        const double clipped_fill = (effective_fill_probability < min_fill_probability)
                                        ? min_fill_probability
                                        : effective_fill_probability;
        const double fill_penalty = regime.fill_weight_bps * (1.0 - clipped_fill);
        const double queue_penalty = regime.queue_weight_bps * venue_quote.queue_ahead_qty;
        const double toxicity_penalty = regime.toxicity_weight * venue_quote.toxicity_bps;
        return base_cost + fill_penalty + queue_penalty + toxicity_penalty +
               venue_quote.taker_markout_bps + venue_quote.quality_penalty_bps;
    }

    auto SmartOrderRouter::expected_shortfall_bps(const VenueQuote &venue_quote, Side side,
                                                  const RoutingRegime &regime, double best_px,
                                                  double inventory_age_penalty_bps,
                                                  double min_fill_probability) noexcept -> double {
        const double quote_px = quote_price_for_side(venue_quote, side);
        if (quote_px <= 0.0 || best_px <= 0.0) {
            return k_infinite_price;
        }
        const double price_penalty_bps =
                lower_price_is_better(side) ? ((quote_px - best_px) / best_px) * 1e4
                                            : ((best_px - quote_px) / best_px) * 1e4;
        return score_venue_bps(venue_quote, side, regime, min_fill_probability) + price_penalty_bps +
               inventory_age_penalty_bps;
    }

    auto SmartOrderRouter::route(Side side, double quantity,
                                 const std::array<VenueQuote, MAX_VENUES> &venues,
                                 const SmartOrderRouterConfig &sor_cfg) noexcept
        -> RoutingDecision {
        RoutingDecision out;

        std::array<bool, MAX_VENUES> used{};
        double remaining = quantity;
        const RoutingRegime regime = infer_regime(venues, sor_cfg);
        const double best_px = find_best_price(side, venues);
        if (best_px >= k_missing_best_price) {
            return out;
        }

        while (remaining > k_min_remaining_qty && out.child_count < out.children.size()) {
            const int best_idx = find_best_venue_index(venues, used, side, regime, best_px,
                                                       sor_cfg.min_fill_probability);
            if (best_idx < 0) {
                break;
            }
            append_child_order(out, side, venues[best_idx], remaining, used, best_idx);
        }

        return out;
    }

    auto SmartOrderRouter::route_with_alpha(Side side, double base_quantity,
                                            const AlphaSignal &alpha_signal,
                                            const std::array<VenueQuote, MAX_VENUES> &venues,
                                            const RoutingConstraints &cfg,
                                            const SmartOrderRouterConfig &sor_cfg) noexcept -> RoutingDecision {
        RoutingDecision out;

        if (alpha_signal.risk_score >= cfg.alpha_risk_max) {
            out.blocked_by_alpha = true;
            return out;
        }

        const bool direction_allowed =
            (side == Side::BID && alpha_signal.signal_bps >= cfg.alpha_min_signal_bps) ||
            (side == Side::ASK && alpha_signal.signal_bps <= -cfg.alpha_min_signal_bps);
        if (!direction_allowed) {
            out.blocked_by_alpha = true;
            return out;
        }

        const double signal_mag = std::abs(alpha_signal.signal_bps);
        const double scale = 1.0 + (cfg.alpha_qty_scale * signal_mag);
        return route(side, base_quantity * scale, venues, sor_cfg);
    }
}
