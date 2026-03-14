#include "smart_order_router.hpp"

namespace trading {

double SmartOrderRouter::effective_price_bps(const VenueQuote& v, Side side) noexcept {
    const double px = (side == Side::BID) ? v.best_ask : v.best_bid;
    if (px <= 0.0) return 1e12;

    const double px_component = px * 1e4;
    return px_component + v.taker_fee_bps + v.latency_penalty_bps + v.risk_penalty_bps;
}

RoutingDecision SmartOrderRouter::route(Side side,
                                        double quantity,
                                        const std::array<VenueQuote, MAX_VENUES>& venues) const noexcept {
    RoutingDecision out;

    std::array<bool, MAX_VENUES> used{};
    double remaining = quantity;

    while (remaining > 1e-12 && out.child_count < out.children.size()) {
        int best_idx = -1;
        double best_score = 1e18;

        for (size_t i = 0; i < venues.size(); ++i) {
            if (used[i]) continue;
            const auto& v = venues[i];
            if (!v.healthy || v.depth_qty <= 0.0) continue;

            const double score = effective_price_bps(v, side);
            if (score < best_score) {
                best_score = score;
                best_idx = static_cast<int>(i);
            }
        }

        if (best_idx < 0) break;

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

}  // namespace trading
