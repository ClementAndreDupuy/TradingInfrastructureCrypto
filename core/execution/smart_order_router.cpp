#include "smart_order_router.hpp"

#include <cmath>

namespace trading {
namespace {

constexpr double k_infinite_price = 1e18;
constexpr double k_missing_best_price = 1e17;
constexpr double k_min_remaining_qty = 1e-12;

auto quote_price_for_side(const VenueQuote& venue_quote, Side side) noexcept -> double {
    return (side == Side::BID) ? venue_quote.best_ask : venue_quote.best_bid;
}

auto find_best_price(Side side, const std::array<VenueQuote, SmartOrderRouter::MAX_VENUES>& venues)
    -> double {
    double best_px = k_infinite_price;
    for (const auto& venue_quote : venues) {
        if (!venue_quote.healthy || venue_quote.depth_qty <= 0.0) {
            continue;
        }
        const double quote_price = quote_price_for_side(venue_quote, side);
        if (quote_price > 0.0 && quote_price < best_px) {
            best_px = quote_price;
        }
    }
    return best_px;
}

auto find_best_venue_index(const std::array<VenueQuote, SmartOrderRouter::MAX_VENUES>& venues,
                           const std::array<bool, SmartOrderRouter::MAX_VENUES>& used, Side side,
                           const RoutingRegime& regime, double best_px) noexcept -> int {
    int best_idx = -1;
    double best_score = k_infinite_price;

    for (size_t i = 0; i < venues.size(); ++i) {
        if (used[i]) {
            continue;
        }
        const auto& venue_quote = venues[i];
        if (!venue_quote.healthy || venue_quote.depth_qty <= 0.0) {
            continue;
        }

        const double quote_px = quote_price_for_side(venue_quote, side);
        const double base_cost = venue_quote.taker_fee_bps + venue_quote.latency_penalty_bps +
                                 venue_quote.risk_penalty_bps;
        const double clipped_fill =
            (venue_quote.fill_probability < 0.05) ? 0.05 : venue_quote.fill_probability;
        const double fill_penalty = regime.fill_weight_bps * (1.0 - clipped_fill);
        const double queue_penalty = regime.queue_weight_bps * venue_quote.queue_ahead_qty;
        const double toxicity_penalty = regime.toxicity_weight * venue_quote.toxicity_bps;
        const double px_penalty_bps = ((quote_px - best_px) / best_px) * 1e4;
        const double score =
            base_cost + fill_penalty + queue_penalty + toxicity_penalty + px_penalty_bps;
        if (score < best_score) {
            best_score = score;
            best_idx = static_cast<int>(i);
        }
    }

    return best_idx;
}

void append_child_order(RoutingDecision& out, Side side, const VenueQuote& winner,
                        double& remaining, std::array<bool, SmartOrderRouter::MAX_VENUES>& used,
                        int best_idx) noexcept {
    const double clip = (remaining < winner.depth_qty) ? remaining : winner.depth_qty;

    ChildOrder child;
    child.exchange = winner.exchange;
    child.quantity = clip;
    child.limit_price = quote_price_for_side(winner, side);
    out.children[out.child_count++] = child;

    remaining -= clip;
    used[best_idx] = true;
}

} // namespace

auto SmartOrderRouter::effective_price_bps(const VenueQuote& venue_quote, Side side) noexcept
    -> double {
    const double quote_price = (side == Side::BID) ? venue_quote.best_ask : venue_quote.best_bid;
    if (quote_price <= 0.0) {
        return 1e12;
    }

    return venue_quote.taker_fee_bps + venue_quote.latency_penalty_bps +
           venue_quote.risk_penalty_bps;
}

auto SmartOrderRouter::infer_regime(const std::array<VenueQuote, MAX_VENUES>& venues) noexcept
    -> RoutingRegime {
    double healthy_count = 0.0;
    double total_toxicity = 0.0;
    double total_fill = 0.0;

    for (const auto& venue_quote : venues) {
        if (!venue_quote.healthy) {
            continue;
        }
        healthy_count += 1.0;
        total_toxicity += venue_quote.toxicity_bps;
        total_fill += venue_quote.fill_probability;
    }

    RoutingRegime regime;
    if (healthy_count <= 0.0) {
        return regime;
    }

    const double avg_toxicity = total_toxicity / healthy_count;
    const double avg_fill = total_fill / healthy_count;

    if (avg_toxicity >= 2.0) {
        regime.fill_weight_bps = 7.0;
        regime.queue_weight_bps = 1.1;
        regime.toxicity_weight = 3.2;
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

auto SmartOrderRouter::score_venue_bps(const VenueQuote& venue_quote, Side side,
                                       const RoutingRegime& regime) noexcept -> double {
    const double base_cost = effective_price_bps(venue_quote, side);
    const double clipped_fill =
        (venue_quote.fill_probability < 0.05) ? 0.05 : venue_quote.fill_probability;
    const double fill_penalty = regime.fill_weight_bps * (1.0 - clipped_fill);
    const double queue_penalty = regime.queue_weight_bps * venue_quote.queue_ahead_qty;
    const double toxicity_penalty = regime.toxicity_weight * venue_quote.toxicity_bps;
    return base_cost + fill_penalty + queue_penalty + toxicity_penalty;
}

auto SmartOrderRouter::route(Side side, double quantity,
                             const std::array<VenueQuote, MAX_VENUES>& venues) noexcept
    -> RoutingDecision {
    RoutingDecision out;

    std::array<bool, MAX_VENUES> used{};
    double remaining = quantity;
    const RoutingRegime regime = infer_regime(venues);
    const double best_px = find_best_price(side, venues);
    if (best_px >= k_missing_best_price) {
        return out;
    }

    while (remaining > k_min_remaining_qty && out.child_count < out.children.size()) {
        const int best_idx = find_best_venue_index(venues, used, side, regime, best_px);
        if (best_idx < 0) {
            break;
        }
        append_child_order(out, side, venues[best_idx], remaining, used, best_idx);
    }

    return out;
}

auto SmartOrderRouter::route_with_alpha(Side side, double base_quantity,
                                        const AlphaSignal& alpha_signal,
                                        const std::array<VenueQuote, MAX_VENUES>& venues,
                                        const RoutingConstraints& cfg) noexcept -> RoutingDecision {
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
    return route(side, base_quantity * scale, venues);
}

} // namespace trading
