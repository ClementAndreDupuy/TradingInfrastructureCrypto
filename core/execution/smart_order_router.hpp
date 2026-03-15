#pragma once

#include "../common/types.hpp"
#include "../ipc/alpha_signal.hpp"

#include <array>
#include <cstddef>

namespace trading {

struct VenueQuote {
    Exchange exchange = Exchange::UNKNOWN;
    double best_bid = 0.0;
    double best_ask = 0.0;
    double depth_qty = 0.0;
    double taker_fee_bps = 0.0;
    double latency_penalty_bps = 0.0;
    double risk_penalty_bps = 0.0;
    bool healthy = false;
};

struct ChildOrder {
    Exchange exchange = Exchange::UNKNOWN;
    double quantity = 0.0;
    double limit_price = 0.0;
};

struct RoutingDecision {
    std::array<ChildOrder, 8> children{};
    size_t child_count = 0;
    bool blocked_by_alpha = false;
};

struct RoutingConstraints {
    double alpha_min_signal_bps = 3.0;
    double alpha_risk_max = 0.65;
    double alpha_qty_scale = 0.10;
};

class SmartOrderRouter {
  public:
    static constexpr size_t MAX_VENUES = 4;

    RoutingDecision route(Side side, double quantity,
                          const std::array<VenueQuote, MAX_VENUES>& venues) const noexcept;

    RoutingDecision route_with_alpha(Side side, double base_quantity,
                                     const AlphaSignal& alpha_signal,
                                     const std::array<VenueQuote, MAX_VENUES>& venues,
                                     const RoutingConstraints& cfg = {}) const noexcept;

  private:
    static double effective_price_bps(const VenueQuote& v, Side side) noexcept;
};

} // namespace trading
