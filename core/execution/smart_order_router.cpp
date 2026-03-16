#include "smart_order_router.hpp"

#include <cmath>

namespace trading {

double SmartOrderRouter::effective_price_bps(const VenueQuote& v, Side side) noexcept {
    const double px = (side == Side::BID) ? v.best_ask : v.best_bid;
    if (px <= 0.0)
        return 1e12;

    const double px_component = px * 1e4;
    return px_component + v.taker_fee_bps + v.latency_penalty_bps + v.risk_penalty_bps;
}

RoutingRegime
SmartOrderRouter::infer_regime(const std::array<VenueQuote, MAX_VENUES>& venues) noexcept {
    double healthy_count = 0.0;
    double total_toxicity = 0.0;
    double total_fill = 0.0;

    for (const auto& v : venues) {
        if (!v.healthy)
            continue;
        healthy_count += 1.0;
        total_toxicity += v.toxicity_bps;
        total_fill += v.fill_probability;
    }

    RoutingRegime regime;
    if (healthy_count <= 0.0)
        return regime;

    const double avg_toxicity = total_toxicity / healthy_count;
    const double avg_fill = total_fill / healthy_count;

    if (avg_toxicity >= 2.0) {
        regime.fill_weight_bps = 7.0;
        regime.queue_weight_bps = 1.1;
        regime.toxicity_weight = 1.7;
        return regime;
    }

    if (avg_fill < 0.45) {
        regime.fill_weight_bps = 6.0;
        regime.queue_weight_bps = 0.9;
        regime.toxicity_weight = 1.2;
        return regime;
    }

    if (avg_fill > 0.75) {
        regime.fill_weight_bps = 3.0;
        regime.queue_weight_bps = 0.4;
        regime.toxicity_weight = 0.8;
    }

    return regime;
}

double SmartOrderRouter::score_venue_bps(const VenueQuote& v, Side side,
                                         const RoutingRegime& regime) noexcept {
    const double base_cost = effective_price_bps(v, side);
    const double clipped_fill = (v.fill_probability < 0.05) ? 0.05 : v.fill_probability;
    const double fill_penalty = regime.fill_weight_bps * (1.0 - clipped_fill);
    const double queue_penalty = regime.queue_weight_bps * v.queue_ahead_qty;
    const double toxicity_penalty = regime.toxicity_weight * v.toxicity_bps;
    return base_cost + fill_penalty + queue_penalty + toxicity_penalty;
}

RoutingDecision
SmartOrderRouter::route(Side side, double quantity,
                        const std::array<VenueQuote, MAX_VENUES>& venues) const noexcept {
    RoutingDecision out;

    std::array<bool, MAX_VENUES> used{};
    double remaining = quantity;
    const RoutingRegime regime = infer_regime(venues);

    while (remaining > 1e-12 && out.child_count < out.children.size()) {
        int best_idx = -1;
        double best_score = 1e18;

        for (size_t i = 0; i < venues.size(); ++i) {
            if (used[i])
                continue;
            const auto& v = venues[i];
            if (!v.healthy || v.depth_qty <= 0.0)
                continue;

            const double score = score_venue_bps(v, side, regime);
            if (score < best_score) {
                best_score = score;
                best_idx = static_cast<int>(i);
            }
        }

        if (best_idx < 0)
            break;

        const VenueQuote& winner = venues[best_idx];
        const double clip = (remaining < winner.depth_qty) ? remaining : winner.depth_qty;

        ChildOrder child;
        child.exchange = winner.exchange;
        child.quantity = clip;
        child.limit_price = (side == Side::BID) ? winner.best_ask : winner.best_bid;
        out.children[out.child_count++] = child;

        remaining -= clip;
        used[best_idx] = true;
    }

    return out;
}

RoutingDecision SmartOrderRouter::route_with_alpha(Side side, double base_quantity,
                                                   const AlphaSignal& alpha_signal,
                                                   const std::array<VenueQuote, MAX_VENUES>& venues,
                                                   const RoutingConstraints& cfg) const noexcept {
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
    const double scale = 1.0 + cfg.alpha_qty_scale * signal_mag;
    return route(side, base_quantity * scale, venues);
}

} // namespace trading
