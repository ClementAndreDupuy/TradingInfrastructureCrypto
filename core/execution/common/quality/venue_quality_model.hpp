#pragma once

#include "../../router/smart_order_router.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace trading {
    struct VenueQualitySnapshot {
        Exchange exchange = Exchange::UNKNOWN;
        double fill_probability = 0.5;
        double passive_markout_bps = 0.0;
        double taker_markout_bps = 0.0;
        double reject_rate = 0.0;
        double cancel_latency_penalty_bps = 0.0;
        double health_penalty_bps = 0.0;
        double stability_penalty_bps = 0.0;
        double composite_penalty_bps = 0.0;
        double composite_fill_probability = 0.5;
        uint64_t sample_count = 0;
    };

    class VenueQualityModel {
    public:
        struct Config {
            double min_fill_probability = 0.10;
            double max_fill_probability = 0.98;
            double fill_probability_alpha = 0.18;
            double markout_alpha = 0.12;
            double reject_rate_alpha = 0.10;
            double cancel_latency_alpha = 0.15;
            double health_alpha = 0.20;
            double max_fill_step = 0.08;
            double max_markout_step_bps = 0.40;
            double max_reject_step = 0.06;
            double max_cancel_latency_step_bps = 0.35;
            double max_health_step_bps = 0.50;
            double reject_rate_penalty_bps = 6.0;
            double cancel_latency_penalty_scale_bps = 3.0;
            double health_penalty_scale_bps = 8.0;
            double passive_markout_penalty_scale = 1.0;
            double taker_markout_penalty_scale = 1.0;
            double stability_penalty_scale = 0.75;
        };

        VenueQualityModel() = default;

        explicit VenueQualityModel(Config cfg) : cfg_(cfg) {
        }

        void observe_fill_probability(Exchange exchange, double realized_fill_probability) noexcept {
            auto &state = state_for(exchange);
            update_clamped(state.fill_probability, realized_fill_probability, cfg_.fill_probability_alpha,
                           cfg_.max_fill_step, cfg_.min_fill_probability, cfg_.max_fill_probability);
            ++state.sample_count;
            refresh_snapshot(state, exchange);
        }

        void observe_markout(Exchange exchange, bool passive, double markout_bps) noexcept {
            auto &state = state_for(exchange);
            if (passive) {
                update_clamped(state.passive_markout_bps, markout_bps, cfg_.markout_alpha,
                               cfg_.max_markout_step_bps, -10.0, 10.0);
            } else {
                update_clamped(state.taker_markout_bps, markout_bps, cfg_.markout_alpha,
                               cfg_.max_markout_step_bps, -10.0, 10.0);
            }
            ++state.sample_count;
            refresh_snapshot(state, exchange);
        }

        void observe_reject(Exchange exchange, bool rejected) noexcept {
            auto &state = state_for(exchange);
            const double sample = rejected ? 1.0 : 0.0;
            update_clamped(state.reject_rate, sample, cfg_.reject_rate_alpha, cfg_.max_reject_step, 0.0,
                           1.0);
            ++state.sample_count;
            refresh_snapshot(state, exchange);
        }

        void observe_cancel_latency(Exchange exchange, std::chrono::microseconds latency) noexcept {
            const double ms = std::max(0.0, static_cast<double>(latency.count()) / 1000.0);
            const double normalized_penalty = std::clamp(ms / 10.0, 0.0, 5.0);
            auto &state = state_for(exchange);
            update_clamped(state.cancel_latency_penalty_bps, normalized_penalty,
                           cfg_.cancel_latency_alpha, cfg_.max_cancel_latency_step_bps, 0.0, 5.0);
            ++state.sample_count;
            refresh_snapshot(state, exchange);
        }

        void observe_health(Exchange exchange, bool healthy) noexcept {
            auto &state = state_for(exchange);
            const double penalty = healthy ? 0.0 : 1.0;
            update_clamped(state.health_penalty_bps, penalty, cfg_.health_alpha,
                           cfg_.max_health_step_bps / std::max(1.0, cfg_.health_penalty_scale_bps), 0.0,
                           1.0);
            refresh_snapshot(state, exchange);
        }

        [[nodiscard]] VenueQualitySnapshot snapshot(Exchange exchange) const noexcept {
            const auto &state = state_for(exchange);
            return state.snapshot;
        }

        void apply(VenueQuote &venue) const noexcept {
            if (venue.exchange == Exchange::UNKNOWN) {
                return;
            }
            const VenueQualitySnapshot snap = snapshot(venue.exchange);
            venue.adaptive_fill_probability = snap.composite_fill_probability;
            venue.passive_markout_bps = snap.passive_markout_bps;
            venue.taker_markout_bps = snap.taker_markout_bps;
            venue.reject_rate = snap.reject_rate;
            venue.cancel_latency_penalty_bps = snap.cancel_latency_penalty_bps;
            venue.health_penalty_bps = snap.health_penalty_bps;
            venue.stability_penalty_bps = snap.stability_penalty_bps;
            venue.quality_penalty_bps = snap.composite_penalty_bps;
        }

        template <size_t N>
        void apply(std::array<VenueQuote, N> &venues) const noexcept {
            for (auto &venue: venues) {
                apply(venue);
            }
        }

        void persist_snapshot(FILE *stream, const char *symbol,
                              std::chrono::system_clock::time_point now) const noexcept {
            if (stream == nullptr) {
                return;
            }
            const auto epoch_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      now.time_since_epoch())
                                      .count();
            for (const auto exchange: k_supported_exchanges) {
                const VenueQualitySnapshot snap = snapshot(exchange);
                std::fprintf(stream,
                             "{\"event\":\"VENUE_QUALITY\",\"ts_ns\":%lld,\"symbol\":\"%s\","
                             "\"exchange\":\"%s\",\"fill_probability\":%.4f,"
                             "\"passive_markout_bps\":%.4f,\"taker_markout_bps\":%.4f,"
                             "\"reject_rate\":%.4f,\"cancel_latency_penalty_bps\":%.4f,"
                             "\"health_penalty_bps\":%.4f,\"stability_penalty_bps\":%.4f,"
                             "\"composite_penalty_bps\":%.4f,\"sample_count\":%llu}\n",
                             static_cast<long long>(epoch_ns), symbol, exchange_to_string(exchange),
                             snap.composite_fill_probability, snap.passive_markout_bps,
                             snap.taker_markout_bps, snap.reject_rate, snap.cancel_latency_penalty_bps,
                             snap.health_penalty_bps, snap.stability_penalty_bps,
                             snap.composite_penalty_bps,
                             static_cast<unsigned long long>(snap.sample_count));
            }
            std::fflush(stream);
        }

    private:
        struct VenueState {
            double fill_probability = 0.5;
            double passive_markout_bps = 0.0;
            double taker_markout_bps = 0.0;
            double reject_rate = 0.0;
            double cancel_latency_penalty_bps = 0.0;
            double health_penalty_bps = 0.0;
            VenueQualitySnapshot snapshot{};
            uint64_t sample_count = 0;
        };

        static constexpr std::array<Exchange, 4> k_supported_exchanges{
            Exchange::BINANCE,
            Exchange::OKX,
            Exchange::COINBASE,
            Exchange::KRAKEN,
        };

        Config cfg_{};
        std::array<VenueState, k_supported_exchanges.size()> states_{};

        static size_t index_for(Exchange exchange) noexcept {
            switch (exchange) {
                case Exchange::BINANCE:
                    return 0;
                case Exchange::OKX:
                    return 1;
                case Exchange::COINBASE:
                    return 2;
                case Exchange::KRAKEN:
                    return 3;
                default:
                    return 0;
            }
        }

        VenueState &state_for(Exchange exchange) noexcept {
            return states_[index_for(exchange)];
        }

        const VenueState &state_for(Exchange exchange) const noexcept {
            return states_[index_for(exchange)];
        }

        static void update_clamped(double &value, double sample, double alpha, double max_step,
                                   double min_value, double max_value) noexcept {
            const double delta = std::clamp(alpha * (sample - value), -max_step, max_step);
            value = std::clamp(value + delta, min_value, max_value);
        }

        void refresh_snapshot(VenueState &state, Exchange exchange) noexcept {
            const double reject_penalty = state.reject_rate * cfg_.reject_rate_penalty_bps;
            const double cancel_penalty = state.cancel_latency_penalty_bps *
                                          cfg_.cancel_latency_penalty_scale_bps;
            const double health_penalty = state.health_penalty_bps * cfg_.health_penalty_scale_bps;
            const double stability_penalty =
                std::abs(state.passive_markout_bps - state.taker_markout_bps) *
                cfg_.stability_penalty_scale;
            const double fill_drag = std::clamp(1.0 - state.fill_probability, 0.0, 1.0);
            const double composite_fill_probability =
                std::clamp(state.fill_probability * (1.0 - 0.35 * state.reject_rate) *
                               (1.0 - 0.15 * state.health_penalty_bps),
                           cfg_.min_fill_probability, cfg_.max_fill_probability);

            state.snapshot.exchange = exchange;
            state.snapshot.fill_probability = state.fill_probability;
            state.snapshot.passive_markout_bps = state.passive_markout_bps;
            state.snapshot.taker_markout_bps = state.taker_markout_bps;
            state.snapshot.reject_rate = state.reject_rate;
            state.snapshot.cancel_latency_penalty_bps = cancel_penalty;
            state.snapshot.health_penalty_bps = health_penalty;
            state.snapshot.stability_penalty_bps = stability_penalty;
            state.snapshot.composite_penalty_bps = reject_penalty + cancel_penalty +
                                                   health_penalty + (fill_drag * 4.0) +
                                                   stability_penalty;
            state.snapshot.composite_fill_probability = composite_fill_probability;
            state.snapshot.sample_count = state.sample_count;
        }
    };
}
