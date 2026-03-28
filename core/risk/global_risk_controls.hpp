#pragma once

#include "kill_switch.hpp"

#include "../common/logging.hpp"
#include "../common/types.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace trading {
    struct GlobalRiskConfig {
        double max_gross_notional;
        double max_net_notional;
        double max_symbol_concentration;
        double max_venue_notional;
        double max_cross_venue_net_notional;
        bool kill_on_breach;
    };

    struct FuturesRiskGateSymbolConfig {
        char symbol[16] = {};
        double max_leverage = 0.0;
    };

    struct FuturesRiskGateConfig {
        bool enabled = false;
        double max_projected_funding_cost_bps = 0.0;
        double funding_cost_scale_start_bps = 0.0;
        double min_funding_scale = 1.0;
        double max_mark_index_divergence_bps = 0.0;
        double max_maintenance_margin_ratio = 0.0;
        double default_max_leverage = 0.0;
        std::array<FuturesRiskGateSymbolConfig, 16> symbol_limits = {};
        size_t symbol_limit_count = 0;
    };

    struct FuturesRiskContext {
        double collateral_notional = 0.0;
        double current_abs_notional = 0.0;
        double maintenance_margin_ratio = 0.0;
        double mark_price = 0.0;
        double index_price = 0.0;
        double funding_rate_bps = 0.0;
        double hours_to_funding = 8.0;
    };

    enum class GlobalRiskCheckResult : uint8_t {
        OK = 0,
        INVALID_INPUT = 1,
        GROSS_NOTIONAL_CAP = 2,
        NET_NOTIONAL_CAP = 3,
        CONCENTRATION_CAP = 4,
        VENUE_CAP = 5,
        CROSS_VENUE_NETTING_CAP = 6,
        FUTURES_LEVERAGE_CAP = 7,
        FUTURES_MAINTENANCE_MARGIN_CAP = 8,
        FUTURES_MARK_INDEX_DIVERGENCE_CAP = 9,
        FUTURES_FUNDING_COST_CAP = 10,
    };

    class GlobalRiskControls {
    public:
        static constexpr size_t MAX_SYMBOLS = 32;
        static constexpr size_t NUM_VENUES = 4;
        static_assert(NUM_VENUES == 4, "Update NUM_VENUES to match the number of Exchange enum values");

        GlobalRiskControls(const GlobalRiskConfig &cfg, KillSwitch &kill_switch) noexcept
            : cfg_(cfg), kill_switch_(kill_switch) {
            for (auto &state: symbol_states_)
                state.active.store(false, std::memory_order_relaxed);
        }

        GlobalRiskControls(const GlobalRiskConfig &cfg, const FuturesRiskGateConfig &futures_cfg,
                           KillSwitch &kill_switch) noexcept
            : cfg_(cfg), futures_cfg_(futures_cfg), kill_switch_(kill_switch) {
            for (auto &state: symbol_states_)
                state.active.store(false, std::memory_order_relaxed);
        }

        GlobalRiskCheckResult check_order(Exchange exchange, const char *symbol,
                                          double signed_notional) const noexcept {
            if (!symbol || symbol[0] == '\0' || !std::isfinite(signed_notional) ||
                signed_notional == 0.0)
                return GlobalRiskCheckResult::INVALID_INPUT;

            const double abs_notional = std::abs(signed_notional);
            const double gross_after = gross_notional_.load(std::memory_order_acquire) + abs_notional;
            const double net_after = net_notional_.load(std::memory_order_acquire) + signed_notional;
            if (cfg_.max_gross_notional > 0.0 && gross_after > cfg_.max_gross_notional)
                return GlobalRiskCheckResult::GROSS_NOTIONAL_CAP;
            if (cfg_.max_net_notional > 0.0 && std::abs(net_after) > cfg_.max_net_notional)
                return GlobalRiskCheckResult::NET_NOTIONAL_CAP;

            const int venue_idx = venue_index(exchange);
            if (venue_idx < 0)
                return GlobalRiskCheckResult::INVALID_INPUT;

            const double venue_after =
                    venue_gross_notional_[static_cast<size_t>(venue_idx)].load(std::memory_order_acquire) +
                    abs_notional;
            if (cfg_.max_venue_notional > 0.0 && venue_after > cfg_.max_venue_notional)
                return GlobalRiskCheckResult::VENUE_CAP;

            const SymbolState *symbol_state = find_symbol(symbol);
            const double symbol_gross_before =
                    symbol_state ? symbol_state->gross_notional.load(std::memory_order_acquire) : 0.0;
            const double symbol_gross_after = symbol_gross_before + abs_notional;
            const double symbol_net_after =
                    (symbol_state ? symbol_state->net_notional.load(std::memory_order_acquire) : 0.0) +
                    signed_notional;

            const double gross_before = gross_notional_.load(std::memory_order_acquire);
            const double other_symbol_gross_before = gross_before - symbol_gross_before;
            if (cfg_.max_symbol_concentration > 0.0 && other_symbol_gross_before > 0.0 &&
                gross_after > 0.0) {
                const double concentration = symbol_gross_after / gross_after;
                if (concentration > cfg_.max_symbol_concentration)
                    return GlobalRiskCheckResult::CONCENTRATION_CAP;
            }

            if (cfg_.max_cross_venue_net_notional > 0.0 && symbol_gross_before > 0.0 &&
                std::abs(symbol_net_after) > cfg_.max_cross_venue_net_notional) {
                return GlobalRiskCheckResult::CROSS_VENUE_NETTING_CAP;
            }

            return GlobalRiskCheckResult::OK;
        }

        GlobalRiskCheckResult commit_order(Exchange exchange, const char *symbol,
                                           double signed_notional) noexcept {
            const GlobalRiskCheckResult check = check_order(exchange, symbol, signed_notional);
            if (check != GlobalRiskCheckResult::OK) {
                if (cfg_.kill_on_breach)
                    kill_switch_.trigger(KillReason::CIRCUIT_BREAKER);
                LOG_ERROR("Global risk breach", "check", result_to_string(check), "symbol", symbol,
                          "exchange", exchange_to_string(exchange), "signed_notional", signed_notional);
                return check;
            }

            SymbolState *state = get_or_create_symbol(symbol);
            if (!state)
                return GlobalRiskCheckResult::INVALID_INPUT;

            const double abs_notional = std::abs(signed_notional);
            atomic_add(gross_notional_, abs_notional);
            atomic_add(net_notional_, signed_notional);

            const int venue_idx = venue_index(exchange);
            atomic_add(venue_gross_notional_[static_cast<size_t>(venue_idx)], abs_notional);
            atomic_add(state->gross_notional, abs_notional);
            atomic_add(state->net_notional, signed_notional);
            return GlobalRiskCheckResult::OK;
        }

        GlobalRiskCheckResult check_futures_order(Exchange exchange, const char *symbol,
                                                  double signed_notional,
                                                  const FuturesRiskContext &ctx,
                                                  double &scaled_notional) const noexcept {
            scaled_notional = signed_notional;
            const GlobalRiskCheckResult gate = check_futures_gate(symbol, signed_notional, ctx,
                                                                  scaled_notional);
            if (gate != GlobalRiskCheckResult::OK)
                return gate;
            return check_order(exchange, symbol, scaled_notional);
        }

        GlobalRiskCheckResult commit_futures_order(Exchange exchange, const char *symbol,
                                                   double signed_notional,
                                                   const FuturesRiskContext &ctx,
                                                   double &scaled_notional) noexcept {
            const GlobalRiskCheckResult check =
                check_futures_order(exchange, symbol, signed_notional, ctx, scaled_notional);
            if (check != GlobalRiskCheckResult::OK) {
                if (cfg_.kill_on_breach)
                    kill_switch_.trigger(KillReason::CIRCUIT_BREAKER);
                LOG_ERROR("Global futures risk breach", "check", result_to_string(check), "symbol",
                          symbol, "exchange", exchange_to_string(exchange), "signed_notional",
                          signed_notional, "scaled_notional", scaled_notional);
                return check;
            }
            return commit_order(exchange, symbol, scaled_notional);
        }

        double gross_notional() const noexcept {
            return gross_notional_.load(std::memory_order_acquire);
        }

        double net_notional() const noexcept { return net_notional_.load(std::memory_order_acquire); }

        static const char *result_to_string(GlobalRiskCheckResult r) noexcept {
            switch (r) {
                case GlobalRiskCheckResult::OK:
                    return "OK";
                case GlobalRiskCheckResult::INVALID_INPUT:
                    return "INVALID_INPUT";
                case GlobalRiskCheckResult::GROSS_NOTIONAL_CAP:
                    return "GROSS_NOTIONAL_CAP";
                case GlobalRiskCheckResult::NET_NOTIONAL_CAP:
                    return "NET_NOTIONAL_CAP";
                case GlobalRiskCheckResult::CONCENTRATION_CAP:
                    return "CONCENTRATION_CAP";
                case GlobalRiskCheckResult::VENUE_CAP:
                    return "VENUE_CAP";
                case GlobalRiskCheckResult::CROSS_VENUE_NETTING_CAP:
                    return "CROSS_VENUE_NETTING_CAP";
                case GlobalRiskCheckResult::FUTURES_LEVERAGE_CAP:
                    return "FUTURES_LEVERAGE_CAP";
                case GlobalRiskCheckResult::FUTURES_MAINTENANCE_MARGIN_CAP:
                    return "FUTURES_MAINTENANCE_MARGIN_CAP";
                case GlobalRiskCheckResult::FUTURES_MARK_INDEX_DIVERGENCE_CAP:
                    return "FUTURES_MARK_INDEX_DIVERGENCE_CAP";
                case GlobalRiskCheckResult::FUTURES_FUNDING_COST_CAP:
                    return "FUTURES_FUNDING_COST_CAP";
                default:
                    return "UNKNOWN";
            }
        }

    private:
        struct SymbolState {
            std::atomic<bool> active{false};
            char symbol[16];
            std::atomic<double> gross_notional{0.0};
            std::atomic<double> net_notional{0.0};
        };

        GlobalRiskConfig cfg_;
        FuturesRiskGateConfig futures_cfg_{};
        KillSwitch &kill_switch_;

        std::atomic<double> gross_notional_{0.0};
        std::atomic<double> net_notional_{0.0};
        std::array<std::atomic<double>, NUM_VENUES> venue_gross_notional_{};
        std::array<SymbolState, MAX_SYMBOLS> symbol_states_{};
        std::atomic<size_t> next_slot_{0};

        static void atomic_add(std::atomic<double> &target, double delta) noexcept {
            double current = target.load(std::memory_order_acquire);
            while (!target.compare_exchange_weak(current, current + delta, std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
            }
        }

        static int venue_index(Exchange exchange) noexcept {
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
                    return -1;
            }
        }

        const SymbolState *find_symbol(const char *symbol) const noexcept {
            const size_t used = next_slot_.load(std::memory_order_acquire);
            for (size_t i = 0; i < used; ++i) {
                const SymbolState &state = symbol_states_[i];
                if (!state.active.load(std::memory_order_acquire))
                    continue;
                if (std::strncmp(state.symbol, symbol, sizeof(state.symbol)) == 0)
                    return &state;
            }
            return nullptr;
        }

        SymbolState *get_or_create_symbol(const char *symbol) noexcept {
            const size_t used = next_slot_.load(std::memory_order_acquire);
            for (size_t i = 0; i < used; ++i) {
                SymbolState &state = symbol_states_[i];
                if (state.active.load(std::memory_order_acquire) &&
                    std::strncmp(state.symbol, symbol, sizeof(state.symbol)) == 0) {
                    return &state;
                }
            }

            const size_t idx = next_slot_.fetch_add(1, std::memory_order_acq_rel);
            if (idx >= MAX_SYMBOLS) {
                LOG_ERROR("Global risk symbol table exhausted", "max_symbols", MAX_SYMBOLS);
                return nullptr;
            }

            SymbolState &state = symbol_states_[idx];
            std::strncpy(state.symbol, symbol, sizeof(state.symbol) - 1);
            state.symbol[sizeof(state.symbol) - 1] = '\0';
            state.active.store(true, std::memory_order_release);
            return &state;
        }

        double max_leverage_for_symbol(const char *symbol) const noexcept {
            for (size_t i = 0; i < futures_cfg_.symbol_limit_count; ++i) {
                if (std::strncmp(futures_cfg_.symbol_limits[i].symbol, symbol,
                                 sizeof(futures_cfg_.symbol_limits[i].symbol)) == 0)
                    return futures_cfg_.symbol_limits[i].max_leverage;
            }
            return futures_cfg_.default_max_leverage;
        }

        GlobalRiskCheckResult check_futures_gate(const char *symbol, double signed_notional,
                                                 const FuturesRiskContext &ctx,
                                                 double &scaled_notional) const noexcept {
            if (!futures_cfg_.enabled)
                return GlobalRiskCheckResult::OK;
            if (!symbol || symbol[0] == '\0')
                return GlobalRiskCheckResult::INVALID_INPUT;
            if (!std::isfinite(ctx.collateral_notional) || ctx.collateral_notional <= 0.0 ||
                !std::isfinite(ctx.current_abs_notional) || ctx.current_abs_notional < 0.0 ||
                !std::isfinite(ctx.mark_price) || ctx.mark_price <= 0.0 || !std::isfinite(ctx.index_price) ||
                ctx.index_price <= 0.0 || !std::isfinite(ctx.maintenance_margin_ratio) ||
                ctx.maintenance_margin_ratio < 0.0 || !std::isfinite(ctx.funding_rate_bps) ||
                !std::isfinite(ctx.hours_to_funding) || ctx.hours_to_funding < 0.0)
                return GlobalRiskCheckResult::INVALID_INPUT;

            if (futures_cfg_.max_maintenance_margin_ratio > 0.0 &&
                ctx.maintenance_margin_ratio > futures_cfg_.max_maintenance_margin_ratio) {
                return GlobalRiskCheckResult::FUTURES_MAINTENANCE_MARGIN_CAP;
            }

            if (futures_cfg_.max_mark_index_divergence_bps > 0.0) {
                const double divergence_bps = std::abs(ctx.mark_price - ctx.index_price) /
                                              ctx.index_price * 1e4;
                if (divergence_bps > futures_cfg_.max_mark_index_divergence_bps)
                    return GlobalRiskCheckResult::FUTURES_MARK_INDEX_DIVERGENCE_CAP;
            }

            const double max_leverage = max_leverage_for_symbol(symbol);
            if (max_leverage > 0.0) {
                const double projected_abs_notional = ctx.current_abs_notional + std::abs(signed_notional);
                const double projected_leverage = projected_abs_notional / ctx.collateral_notional;
                if (projected_leverage > max_leverage)
                    return GlobalRiskCheckResult::FUTURES_LEVERAGE_CAP;
            }

            const double projected_funding_cost_bps =
                std::abs(ctx.funding_rate_bps) * (ctx.hours_to_funding / 8.0);
            if (futures_cfg_.max_projected_funding_cost_bps > 0.0 &&
                projected_funding_cost_bps > futures_cfg_.max_projected_funding_cost_bps) {
                return GlobalRiskCheckResult::FUTURES_FUNDING_COST_CAP;
            }

            if (futures_cfg_.funding_cost_scale_start_bps > 0.0 &&
                futures_cfg_.max_projected_funding_cost_bps >
                    futures_cfg_.funding_cost_scale_start_bps &&
                projected_funding_cost_bps > futures_cfg_.funding_cost_scale_start_bps) {
                const double span = futures_cfg_.max_projected_funding_cost_bps -
                                    futures_cfg_.funding_cost_scale_start_bps;
                const double t =
                    std::clamp((projected_funding_cost_bps - futures_cfg_.funding_cost_scale_start_bps) /
                                   span,
                               0.0, 1.0);
                const double scale = 1.0 - t * (1.0 - futures_cfg_.min_funding_scale);
                scaled_notional = signed_notional * std::clamp(scale, futures_cfg_.min_funding_scale, 1.0);
            }

            return GlobalRiskCheckResult::OK;
        }
    };
}
