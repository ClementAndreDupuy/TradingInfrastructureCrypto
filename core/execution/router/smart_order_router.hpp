#pragma once

#include "../../common/types.hpp"
#include "../../ipc/alpha_signal.hpp"

#include <array>
#include <cstddef>

namespace trading {
    struct VenueQuote {
        Exchange exchange;
        double best_bid;
        double best_ask;
        double depth_qty;
        double taker_fee_bps;
        double latency_penalty_bps;
        double risk_penalty_bps;
        double fill_probability;
        double queue_ahead_qty;
        double toxicity_bps;
        bool healthy;
        bool enabled;
        double adaptive_fill_probability;
        double passive_markout_bps;
        double taker_markout_bps;
        double reject_rate;
        double cancel_latency_penalty_bps;
        double health_penalty_bps;
        double stability_penalty_bps;
        double quality_penalty_bps;
    };

    struct RoutingRegime {
        double fill_weight_bps;
        double queue_weight_bps;
        double toxicity_weight;
    };

    struct SmartOrderRouterConfig {
        double high_toxicity_threshold;
        double low_fill_threshold;
        double high_fill_threshold;
        double high_toxicity_fill_weight;
        double high_toxicity_queue_weight;
        double high_toxicity_tox_weight;
        double low_fill_fill_weight;
        double low_fill_queue_weight;
        double low_fill_tox_weight;
        double high_fill_fill_weight;
        double high_fill_queue_weight;
        double high_fill_tox_weight;
        double min_fill_probability;
    };

    struct ChildOrder {
        Exchange exchange;
        double quantity;
        double limit_price;
        TimeInForce tif;
    };

    struct RoutingDecision {
        std::array<ChildOrder, 8> children{};
        size_t child_count;
        bool blocked_by_alpha;
    };

    struct RoutingConstraints {
        double alpha_min_signal_bps;
        double alpha_risk_max;
        double alpha_qty_scale;
    };

    class SmartOrderRouter {
    public:
        static constexpr size_t MAX_VENUES = 4;

        static RoutingDecision route(Side side, double quantity,
                                     const std::array<VenueQuote, MAX_VENUES> &venues,
                                     const SmartOrderRouterConfig &sor_cfg) noexcept;

        static RoutingDecision route_with_alpha(Side side, double base_quantity,
                                                const AlphaSignal &alpha_signal,
                                                const std::array<VenueQuote, MAX_VENUES> &venues,
                                                const RoutingConstraints &cfg,
                                                const SmartOrderRouterConfig &sor_cfg) noexcept;

        static double effective_price_bps(const VenueQuote &v, Side side) noexcept;

        static RoutingRegime infer_regime(const std::array<VenueQuote, MAX_VENUES> &venues,
                                          const SmartOrderRouterConfig &sor_cfg) noexcept;

        static double score_venue_bps(const VenueQuote &v, Side side,
                                      const RoutingRegime &regime,
                                      double min_fill_probability) noexcept;

        static double expected_shortfall_bps(const VenueQuote &v, Side side,
                                             const RoutingRegime &regime, double best_px_bps,
                                             double inventory_age_penalty_bps,
                                             double min_fill_probability) noexcept;
    };
}
