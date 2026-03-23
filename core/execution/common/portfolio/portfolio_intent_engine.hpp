#pragma once

#include "position_ledger.hpp"
#include "../../router/smart_order_router.hpp"
#include "../../../ipc/alpha_signal.hpp"
#include "../../../ipc/regime_signal.hpp"
#include "../../../shadow/shadow_engine.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace trading {
    enum class PortfolioIntentReasonCode : uint8_t {
        NONE = 0,
        ALPHA_POSITIVE,
        ALPHA_DECAY,
        NEGATIVE_REVERSAL,
        RISK_OFF,
        SHOCK_REGIME,
        ILLIQUID_REGIME,
        STALE_INVENTORY,
        HEALTH_DEGRADED,
        NO_HEALTHY_VENUES,
    };

    struct PortfolioIntentConfig {
        double max_position = 0.80;
        double min_entry_signal_bps = 3.0;
        double alpha_exit_buffer_bps = 0.75;
        double negative_reversal_signal_bps = -1.0;
        double max_risk_score = 0.65;
        double shock_flatten_threshold = 0.60;
        double illiquid_reduce_threshold = 0.55;
        int64_t stale_inventory_ms = 1500;
        double health_reduce_ratio = 0.50;
        bool long_only = true;
    };

    struct PortfolioIntent {
        double target_global_position = 0.0;
        double position_delta = 0.0;
        ShadowUrgency urgency = ShadowUrgency::BALANCED;
        bool flatten_now = false;
        double max_shortfall_bps = 0.0;
        double expected_cost_bps = 0.0;
        std::array<PortfolioIntentReasonCode, 8> reason_codes{};
        size_t reason_count = 0;

        [[nodiscard]] PortfolioIntentReasonCode primary_reason() const noexcept {
            if (reason_count == 0)
                return PortfolioIntentReasonCode::NONE;
            return reason_codes[0];
        }
    };

    class PortfolioIntentEngine {
    public:
        explicit PortfolioIntentEngine(PortfolioIntentConfig cfg = {}) : cfg_(cfg) {
        }

        [[nodiscard]] PortfolioIntent evaluate(
            const AlphaSignal &alpha_signal,
            const RegimeSignal &regime_signal,
            const PositionLedgerSnapshot &ledger,
            const std::array<VenueQuote, SmartOrderRouter::MAX_VENUES> &venues) const noexcept {
            PortfolioIntent out;
            const double current_position = clamp_position(ledger.global_position);
            const double expected_cost_bps = estimate_expected_cost_bps(venues);
            const size_t healthy_venues = count_healthy_venues(venues);
            const size_t enabled_venues = count_enabled_venues(venues);
            const double health_ratio = enabled_venues > 0
                                            ? static_cast<double>(healthy_venues) /
                                                  static_cast<double>(enabled_venues)
                                            : 1.0;
            out.expected_cost_bps = expected_cost_bps;

            if (healthy_venues == 0) {
                append_reason(out, PortfolioIntentReasonCode::NO_HEALTHY_VENUES);
                out.flatten_now = current_position > 0.0;
                out.max_shortfall_bps = expected_cost_bps + 3.0;
                out.urgency = ShadowUrgency::AGGRESSIVE;
                out.position_delta = -current_position;
                return out;
            }

            const bool risk_off = alpha_signal.risk_score >= cfg_.max_risk_score;
            const bool negative_reversal = alpha_signal.signal_bps <= cfg_.negative_reversal_signal_bps;
            const bool shock_regime = regime_signal.p_shock >= cfg_.shock_flatten_threshold;
            const bool illiquid_regime = regime_signal.p_illiquid >= cfg_.illiquid_reduce_threshold;
            const bool stale_inventory = ledger.oldest_inventory_age_ms >= cfg_.stale_inventory_ms &&
                                         current_position > 0.0;
            const bool edge_positive = alpha_signal.signal_bps > expected_cost_bps;

            if (risk_off)
                append_reason(out, PortfolioIntentReasonCode::RISK_OFF);
            if (negative_reversal)
                append_reason(out, PortfolioIntentReasonCode::NEGATIVE_REVERSAL);
            if (shock_regime)
                append_reason(out, PortfolioIntentReasonCode::SHOCK_REGIME);
            if (illiquid_regime)
                append_reason(out, PortfolioIntentReasonCode::ILLIQUID_REGIME);
            if (stale_inventory)
                append_reason(out, PortfolioIntentReasonCode::STALE_INVENTORY);
            if (enabled_venues > 0 && healthy_venues < enabled_venues)
                append_reason(out, PortfolioIntentReasonCode::HEALTH_DEGRADED);

            if (risk_off || negative_reversal || shock_regime) {
                out.flatten_now = current_position > 0.0;
                out.max_shortfall_bps = expected_cost_bps + 2.5;
                out.urgency = ShadowUrgency::AGGRESSIVE;
                out.position_delta = -current_position;
                return out;
            }

            double target = 0.0;
            if (alpha_signal.signal_bps >= cfg_.min_entry_signal_bps && edge_positive) {
                const double alpha_scale = std::clamp(
                    (alpha_signal.signal_bps - expected_cost_bps) /
                    std::max(cfg_.min_entry_signal_bps, 1.0),
                    0.0, 1.0);
                const double risk_scale = std::clamp(1.0 - alpha_signal.risk_score, 0.0, 1.0);
                const double health_scale = (enabled_venues > 0 && healthy_venues < enabled_venues)
                                                ? cfg_.health_reduce_ratio
                                                : 1.0;
                const double regime_scale = std::clamp(
                    1.0 - (0.5 * regime_signal.p_illiquid + 0.35 * regime_signal.p_shock), 0.0, 1.0);
                target = cfg_.max_position * std::clamp(alpha_signal.size_fraction, 0.0, 1.0) *
                         alpha_scale * risk_scale * health_scale * regime_scale;
                append_reason(out, PortfolioIntentReasonCode::ALPHA_POSITIVE);
            } else if (current_position > 0.0) {
                append_reason(out, PortfolioIntentReasonCode::ALPHA_DECAY);
            }

            if (illiquid_regime || stale_inventory) {
                target = std::min(target, current_position * 0.5);
            }

            if (cfg_.long_only)
                target = std::max(0.0, target);

            out.target_global_position = clamp_position(target);
            out.position_delta = out.target_global_position - current_position;
            out.max_shortfall_bps = expected_cost_bps + urgency_buffer_bps(alpha_signal, out.position_delta);
            out.urgency = choose_urgency(alpha_signal, current_position, out.target_global_position,
                                         stale_inventory, healthy_venues);
            out.flatten_now = out.target_global_position <= 1e-9 && current_position > 1e-9 &&
                              out.position_delta < -1e-9;
            return out;
        }

        [[nodiscard]] static const char *reason_code_to_string(PortfolioIntentReasonCode code) noexcept {
            switch (code) {
                case PortfolioIntentReasonCode::ALPHA_POSITIVE:
                    return "alpha_positive";
                case PortfolioIntentReasonCode::ALPHA_DECAY:
                    return "alpha_decay";
                case PortfolioIntentReasonCode::NEGATIVE_REVERSAL:
                    return "negative_reversal";
                case PortfolioIntentReasonCode::RISK_OFF:
                    return "risk_off";
                case PortfolioIntentReasonCode::SHOCK_REGIME:
                    return "shock_regime";
                case PortfolioIntentReasonCode::ILLIQUID_REGIME:
                    return "illiquid_regime";
                case PortfolioIntentReasonCode::STALE_INVENTORY:
                    return "stale_inventory";
                case PortfolioIntentReasonCode::HEALTH_DEGRADED:
                    return "health_degraded";
                case PortfolioIntentReasonCode::NO_HEALTHY_VENUES:
                    return "no_healthy_venues";
                case PortfolioIntentReasonCode::NONE:
                default:
                    return "none";
            }
        }

    private:
        PortfolioIntentConfig cfg_;

        [[nodiscard]] static double estimate_expected_cost_bps(
            const std::array<VenueQuote, SmartOrderRouter::MAX_VENUES> &venues) noexcept {
            double best_cost = 0.0;
            bool found = false;
            for (const auto &venue: venues) {
                if (!venue.healthy || venue.depth_qty <= 0.0)
                    continue;
                const double cost = venue.taker_fee_bps + venue.latency_penalty_bps +
                                    venue.risk_penalty_bps + venue.toxicity_bps +
                                    std::max(0.0, 1.0 - venue.fill_probability) * 2.0;
                if (!found || cost < best_cost) {
                    best_cost = cost;
                    found = true;
                }
            }
            return found ? best_cost : 0.0;
        }

        [[nodiscard]] static size_t count_healthy_venues(
            const std::array<VenueQuote, SmartOrderRouter::MAX_VENUES> &venues) noexcept {
            size_t count = 0;
            for (const auto &venue: venues) {
                if (venue.enabled && venue.healthy && venue.depth_qty > 0.0)
                    ++count;
            }
            return count;
        }

        [[nodiscard]] static size_t count_enabled_venues(
            const std::array<VenueQuote, SmartOrderRouter::MAX_VENUES> &venues) noexcept {
            size_t count = 0;
            for (const auto &venue: venues) {
                if (venue.enabled)
                    ++count;
            }
            return count;
        }

        static void append_reason(PortfolioIntent &intent, PortfolioIntentReasonCode code) noexcept {
            if (code == PortfolioIntentReasonCode::NONE)
                return;
            for (size_t i = 0; i < intent.reason_count; ++i) {
                if (intent.reason_codes[i] == code)
                    return;
            }
            if (intent.reason_count < intent.reason_codes.size())
                intent.reason_codes[intent.reason_count++] = code;
        }

        [[nodiscard]] static double clamp_position(double position) noexcept {
            return std::clamp(position, 0.0, 1.0);
        }

        [[nodiscard]] static double urgency_buffer_bps(const AlphaSignal &alpha_signal,
                                                       double position_delta) noexcept {
            const double delta = std::abs(position_delta);
            if (alpha_signal.horizon_ticks <= 2 || delta >= 0.30)
                return 1.0;
            if (alpha_signal.horizon_ticks >= 8 && delta <= 0.10)
                return 3.0;
            return 2.0;
        }

        [[nodiscard]] static ShadowUrgency choose_urgency(const AlphaSignal &alpha_signal,
                                                          double current_position,
                                                          double target_position,
                                                          bool stale_inventory,
                                                          size_t healthy_venues) noexcept {
            const double delta = std::abs(target_position - current_position);
            if (stale_inventory || healthy_venues <= 1 || alpha_signal.horizon_ticks <= 2 ||
                delta >= 0.30 || alpha_signal.risk_score >= 0.55) {
                return ShadowUrgency::AGGRESSIVE;
            }
            if (alpha_signal.horizon_ticks >= 8 && delta <= 0.10 && alpha_signal.risk_score < 0.35)
                return ShadowUrgency::PASSIVE;
            return ShadowUrgency::BALANCED;
        }
    };
}
