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
        ALPHA_NEGATIVE,
        ALPHA_DECAY,
        NEGATIVE_REVERSAL,
        POSITIVE_REVERSAL,
        RISK_OFF,
        SHOCK_REGIME,
        ILLIQUID_REGIME,
        STALE_INVENTORY,
        STALE_SIGNAL,
        BASIS_TOO_WIDE,
        HEALTH_DEGRADED,
        NO_HEALTHY_VENUES,
    };

    struct PortfolioIntentConfig {
        double max_position;
        double min_entry_signal_bps;
        double alpha_exit_buffer_bps;
        double negative_reversal_signal_bps;
        double positive_reversal_signal_bps;
        double deadband_signal_bps;
        double max_risk_score;
        double shock_enter_threshold;
        double shock_exit_threshold;
        double illiquid_enter_threshold;
        double illiquid_exit_threshold;
        int regime_persistence_ticks;
        int64_t stale_inventory_ms;
        int64_t stale_signal_ms;
        double max_basis_divergence_bps;
        double stale_inventory_alpha_hold_bps;
        double health_reduce_ratio;
        bool long_only;
    };

    struct PortfolioIntentContext {
        double spot_mid_price = 0.0;
        double futures_mid_price = 0.0;
        int64_t signal_age_ms = 0;
    };

    struct PortfolioIntent {
        double target_global_position;
        double position_delta;
        ShadowUrgency urgency;
        bool flatten_now;
        double max_shortfall_bps;
        double expected_cost_bps;
        double basis_slippage_bps;
        double alpha_edge_bps;
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
        explicit PortfolioIntentEngine(PortfolioIntentConfig cfg) : cfg_(cfg) {
        }

        [[nodiscard]] PortfolioIntent evaluate(
            const AlphaSignal &alpha_signal,
            const RegimeSignal &regime_signal,
            const PositionLedgerSnapshot &ledger,
            const std::array<VenueQuote, SmartOrderRouter::MAX_VENUES> &venues,
            const PortfolioIntentContext &ctx = {}) noexcept {
            _update_regime_hysteresis(regime_signal.p_illiquid,
                                      cfg_.illiquid_enter_threshold,
                                      cfg_.illiquid_exit_threshold,
                                      cfg_.regime_persistence_ticks,
                                      illiquid_regime_active_, illiquid_ticks_);
            _update_regime_hysteresis(regime_signal.p_shock,
                                      cfg_.shock_enter_threshold,
                                      cfg_.shock_exit_threshold,
                                      cfg_.regime_persistence_ticks,
                                      shock_regime_active_, shock_ticks_);
            PortfolioIntent out;
            const double current_position = clamp_position(ledger.global_position);
            const double expected_cost_bps = estimate_expected_cost_bps(venues);
            const size_t healthy_venues = count_healthy_venues(venues);
            const size_t enabled_venues = count_enabled_venues(venues);
            out.expected_cost_bps = expected_cost_bps;
            out.basis_slippage_bps = compute_basis_bps(ctx.spot_mid_price, ctx.futures_mid_price);
            out.alpha_edge_bps = alpha_signal.signal_bps - expected_cost_bps;

            if (healthy_venues == 0) {
                append_reason(out, PortfolioIntentReasonCode::NO_HEALTHY_VENUES);
                out.flatten_now = std::abs(current_position) > 0.0;
                out.max_shortfall_bps = expected_cost_bps + 3.0;
                out.urgency = ShadowUrgency::AGGRESSIVE;
                out.position_delta = -current_position;
                return out;
            }

            const bool risk_off = alpha_signal.risk_score >= cfg_.max_risk_score;
            const bool negative_reversal = alpha_signal.signal_bps <= cfg_.negative_reversal_signal_bps;
            const bool positive_reversal = current_position < 0.0 &&
                                           alpha_signal.signal_bps >= cfg_.positive_reversal_signal_bps;
            const bool shock_regime = shock_regime_active_;
            const bool illiquid_regime = illiquid_regime_active_;
            const bool stale_inventory = ledger.oldest_inventory_age_ms >= cfg_.stale_inventory_ms &&
                                         std::abs(current_position) > 0.0;
            const bool stale_signal = cfg_.stale_signal_ms > 0 &&
                                      ctx.signal_age_ms > cfg_.stale_signal_ms;
            const bool basis_too_wide = cfg_.max_basis_divergence_bps > 0.0 &&
                                        out.basis_slippage_bps > cfg_.max_basis_divergence_bps;
            const bool edge_positive = alpha_signal.signal_bps > expected_cost_bps;
            const bool edge_negative = -alpha_signal.signal_bps > expected_cost_bps;

            if (risk_off)
                append_reason(out, PortfolioIntentReasonCode::RISK_OFF);
            if (negative_reversal)
                append_reason(out, PortfolioIntentReasonCode::NEGATIVE_REVERSAL);
            if (positive_reversal)
                append_reason(out, PortfolioIntentReasonCode::POSITIVE_REVERSAL);
            if (shock_regime)
                append_reason(out, PortfolioIntentReasonCode::SHOCK_REGIME);
            if (illiquid_regime)
                append_reason(out, PortfolioIntentReasonCode::ILLIQUID_REGIME);
            if (stale_inventory)
                append_reason(out, PortfolioIntentReasonCode::STALE_INVENTORY);
            if (stale_signal)
                append_reason(out, PortfolioIntentReasonCode::STALE_SIGNAL);
            if (basis_too_wide)
                append_reason(out, PortfolioIntentReasonCode::BASIS_TOO_WIDE);
            if (enabled_venues > 0 && healthy_venues < enabled_venues)
                append_reason(out, PortfolioIntentReasonCode::HEALTH_DEGRADED);

            if (risk_off || negative_reversal || positive_reversal || shock_regime || stale_signal || basis_too_wide) {
                out.flatten_now = std::abs(current_position) > 0.0;
                out.max_shortfall_bps = expected_cost_bps + 2.5;
                out.urgency = ShadowUrgency::AGGRESSIVE;
                out.position_delta = -current_position;
                return out;
            }

            double target = 0.0;
            const bool positive_entry = alpha_signal.signal_bps >= cfg_.min_entry_signal_bps && edge_positive;
            const bool negative_entry = alpha_signal.signal_bps <= -cfg_.min_entry_signal_bps && !cfg_.long_only && edge_negative;
            if (positive_entry || negative_entry) {
                const double alpha_scale = std::clamp(
                    (std::abs(alpha_signal.signal_bps) - expected_cost_bps) /
                    std::max(cfg_.min_entry_signal_bps, 1.0),
                    0.0, 1.0);
                const double risk_scale = std::clamp(1.0 - alpha_signal.risk_score, 0.0, 1.0);
                const double health_scale = (enabled_venues > 0 && healthy_venues < enabled_venues)
                                                ? cfg_.health_reduce_ratio
                                                : 1.0;
                const double regime_scale = std::clamp(
                    1.0 - (0.5 * regime_signal.p_illiquid + 0.35 * regime_signal.p_shock), 0.0, 1.0);
                const double magnitude = cfg_.max_position * std::clamp(alpha_signal.size_fraction, 0.0, 1.0) *
                                         alpha_scale * risk_scale * health_scale * regime_scale;
                target = positive_entry ? magnitude : -magnitude;
                append_reason(out, positive_entry
                                       ? PortfolioIntentReasonCode::ALPHA_POSITIVE
                                       : PortfolioIntentReasonCode::ALPHA_NEGATIVE);
            } else {
                append_reason(out, PortfolioIntentReasonCode::ALPHA_DECAY);
            }

            if (std::abs(alpha_signal.signal_bps) <= cfg_.deadband_signal_bps)
                target = 0.0;

            const bool alpha_strong = std::abs(alpha_signal.signal_bps) >= cfg_.stale_inventory_alpha_hold_bps;
            if (illiquid_regime || (stale_inventory && !alpha_strong)) {
                target = clamp_reduce_target(current_position, target);
            }

            if (cfg_.long_only)
                target = std::max(0.0, target);

            out.target_global_position = clamp_position(target);
            out.position_delta = out.target_global_position - current_position;
            out.max_shortfall_bps = expected_cost_bps + urgency_buffer_bps(alpha_signal, out.position_delta);
            out.urgency = choose_urgency(alpha_signal, current_position, out.target_global_position,
                                         stale_inventory, healthy_venues);
            out.flatten_now = std::abs(out.target_global_position) <= 1e-9 &&
                              std::abs(current_position) > 1e-9 &&
                              std::abs(out.position_delta) > 1e-9;
            return out;
        }

        [[nodiscard]] static const char *reason_code_to_string(PortfolioIntentReasonCode code) noexcept {
            switch (code) {
                case PortfolioIntentReasonCode::ALPHA_POSITIVE:
                    return "alpha_positive";
                case PortfolioIntentReasonCode::ALPHA_NEGATIVE:
                    return "alpha_negative";
                case PortfolioIntentReasonCode::ALPHA_DECAY:
                    return "alpha_decay";
                case PortfolioIntentReasonCode::NEGATIVE_REVERSAL:
                    return "negative_reversal";
                case PortfolioIntentReasonCode::POSITIVE_REVERSAL:
                    return "positive_reversal";
                case PortfolioIntentReasonCode::RISK_OFF:
                    return "risk_off";
                case PortfolioIntentReasonCode::SHOCK_REGIME:
                    return "shock_regime";
                case PortfolioIntentReasonCode::ILLIQUID_REGIME:
                    return "illiquid_regime";
                case PortfolioIntentReasonCode::STALE_INVENTORY:
                    return "stale_inventory";
                case PortfolioIntentReasonCode::STALE_SIGNAL:
                    return "stale_signal";
                case PortfolioIntentReasonCode::BASIS_TOO_WIDE:
                    return "basis_too_wide";
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

        bool illiquid_regime_active_{false};
        int illiquid_ticks_{0};
        bool shock_regime_active_{false};
        int shock_ticks_{0};

        static void _update_regime_hysteresis(double p,
                                              double enter_thresh,
                                              double exit_thresh,
                                              int persistence,
                                              bool &active,
                                              int &ticks) noexcept {
            if (!active) {
                ticks = (p >= enter_thresh) ? ticks + 1 : 0;
                if (ticks >= persistence) {
                    active = true;
                    ticks = 0;
                }
            } else {
                ticks = (p < exit_thresh) ? ticks + 1 : 0;
                if (ticks >= persistence) {
                    active = false;
                    ticks = 0;
                }
            }
        }

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

        [[nodiscard]] static double compute_basis_bps(double spot_mid, double futures_mid) noexcept {
            if (spot_mid <= 0.0 || futures_mid <= 0.0)
                return 0.0;
            return std::abs(futures_mid - spot_mid) / spot_mid * 1e4;
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

        [[nodiscard]] double clamp_position(double position) const noexcept {
            const double min_position = cfg_.long_only ? 0.0 : -cfg_.max_position;
            return std::clamp(position, min_position, cfg_.max_position);
        }

        [[nodiscard]] static double clamp_reduce_target(double current, double target) noexcept {
            if (std::abs(current) <= 1e-9)
                return target;
            if ((current > 0.0 && target < 0.0) || (current < 0.0 && target > 0.0))
                return 0.0;
            return std::abs(target) > std::abs(current) ? current * 0.5 : target;
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
