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
    double max_gross_notional = 0.0;
    double max_net_notional = 0.0;
    double max_symbol_concentration = 1.0;
    double max_venue_notional = 0.0;
    double max_cross_venue_net_notional = 0.0;
    bool kill_on_breach = true;
};

enum class GlobalRiskCheckResult : uint8_t {
    OK = 0,
    INVALID_INPUT = 1,
    GROSS_NOTIONAL_CAP = 2,
    NET_NOTIONAL_CAP = 3,
    CONCENTRATION_CAP = 4,
    VENUE_CAP = 5,
    CROSS_VENUE_NETTING_CAP = 6,
};

class GlobalRiskControls {
  public:
    static constexpr size_t MAX_SYMBOLS = 32;

    GlobalRiskControls(const GlobalRiskConfig& cfg, KillSwitch& kill_switch) noexcept
        : cfg_(cfg), kill_switch_(kill_switch) {
        for (auto& state : symbol_states_)
            state.active.store(false, std::memory_order_relaxed);
    }

    GlobalRiskCheckResult check_order(Exchange exchange, const char* symbol,
                                      double signed_notional) const noexcept {
        if (!symbol || symbol[0] == '\0' || !std::isfinite(signed_notional) || signed_notional == 0.0)
            return GlobalRiskCheckResult::INVALID_INPUT;

        const double abs_notional = std::abs(signed_notional);
        const double gross_after = gross_notional_.load(std::memory_order_acquire) + abs_notional;
        const double net_after =
            net_notional_.load(std::memory_order_acquire) + signed_notional;
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

        const SymbolState* symbol_state = find_symbol(symbol);
        const double symbol_gross_after =
            (symbol_state ? symbol_state->gross_notional.load(std::memory_order_acquire) : 0.0) +
            abs_notional;
        const double symbol_net_after =
            (symbol_state ? symbol_state->net_notional.load(std::memory_order_acquire) : 0.0) +
            signed_notional;

        if (cfg_.max_cross_venue_net_notional > 0.0 &&
            std::abs(symbol_net_after) > cfg_.max_cross_venue_net_notional) {
            return GlobalRiskCheckResult::CROSS_VENUE_NETTING_CAP;
        }

        if (cfg_.max_symbol_concentration > 0.0 && gross_after > 0.0) {
            const double concentration = symbol_gross_after / gross_after;
            if (concentration > cfg_.max_symbol_concentration)
                return GlobalRiskCheckResult::CONCENTRATION_CAP;
        }

        return GlobalRiskCheckResult::OK;
    }

    GlobalRiskCheckResult commit_order(Exchange exchange, const char* symbol,
                                       double signed_notional) noexcept {
        const GlobalRiskCheckResult check = check_order(exchange, symbol, signed_notional);
        if (check != GlobalRiskCheckResult::OK) {
            if (cfg_.kill_on_breach)
                kill_switch_.trigger(KillReason::CIRCUIT_BREAKER);
            LOG_ERROR("Global risk breach", "check", result_to_string(check), "symbol", symbol,
                      "exchange", exchange_to_string(exchange), "signed_notional", signed_notional);
            return check;
        }

        SymbolState* state = get_or_create_symbol(symbol);
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

    double gross_notional() const noexcept { return gross_notional_.load(std::memory_order_acquire); }
    double net_notional() const noexcept { return net_notional_.load(std::memory_order_acquire); }

    static const char* result_to_string(GlobalRiskCheckResult r) noexcept {
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
        default:
            return "UNKNOWN";
        }
    }

  private:
    struct SymbolState {
        std::atomic<bool> active{false};
        char symbol[16] = {};
        std::atomic<double> gross_notional{0.0};
        std::atomic<double> net_notional{0.0};
    };

    GlobalRiskConfig cfg_;
    KillSwitch& kill_switch_;

    std::atomic<double> gross_notional_{0.0};
    std::atomic<double> net_notional_{0.0};
    std::array<std::atomic<double>, 4> venue_gross_notional_{};
    std::array<SymbolState, MAX_SYMBOLS> symbol_states_{};


    static void atomic_add(std::atomic<double>& target, double delta) noexcept {
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

    const SymbolState* find_symbol(const char* symbol) const noexcept {
        for (const auto& state : symbol_states_) {
            if (!state.active.load(std::memory_order_acquire))
                continue;
            if (std::strncmp(state.symbol, symbol, sizeof(state.symbol)) == 0)
                return &state;
        }
        return nullptr;
    }

    SymbolState* get_or_create_symbol(const char* symbol) noexcept {
        for (auto& state : symbol_states_) {
            if (state.active.load(std::memory_order_acquire) &&
                std::strncmp(state.symbol, symbol, sizeof(state.symbol)) == 0) {
                return &state;
            }
        }

        for (auto& state : symbol_states_) {
            bool expected = false;
            if (!state.active.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
                continue;
            std::strncpy(state.symbol, symbol, sizeof(state.symbol) - 1);
            state.symbol[sizeof(state.symbol) - 1] = '\0';
            state.gross_notional.store(0.0, std::memory_order_release);
            state.net_notional.store(0.0, std::memory_order_release);
            return &state;
        }

        LOG_ERROR("Global risk symbol table exhausted", "max_symbols", MAX_SYMBOLS);
        return nullptr;
    }
};

} // namespace trading
