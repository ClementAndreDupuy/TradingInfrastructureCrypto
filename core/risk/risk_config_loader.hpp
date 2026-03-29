#pragma once

#include "circuit_breaker.hpp"
#include "global_risk_controls.hpp"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <array>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace trading {
    struct RiskRuntimeConfig {
        struct VenueCapitalConfig {
            double starting_equity_usd = 0.0;
            double min_free_collateral_buffer_usd = 0.0;
        };

        CircuitBreakerConfig circuit_breaker;
        GlobalRiskConfig global_risk;
        FuturesRiskGateConfig futures_risk;
        int64_t heartbeat_timeout_ns = KillSwitch::DEFAULT_HEARTBEAT_TIMEOUT_NS;
        double target_range_usd = 50.0;
        size_t max_book_levels = 10000;
        double binance_taker_fee_bps = 5.0;
        double kraken_taker_fee_bps = 10.0;
        double okx_taker_fee_bps = 8.0;
        double coinbase_taker_fee_bps = 6.0;
        std::array<VenueCapitalConfig, 4> venue_capital{};
    };

    class RiskConfigLoader {
    public:
        static bool load(const std::string &path, RiskRuntimeConfig &out) {
            std::ifstream in;
            for (const std::string &candidate: candidate_paths(path)) {
                in.open(candidate);
                if (in.is_open())
                    break;
                in.clear();
            }
            if (!in.is_open())
                return false;

            std::unordered_map<std::string, std::string> values;
            std::string line;
            while (std::getline(in, line)) {
                const size_t hash = line.find('#');
                if (hash != std::string::npos)
                    line = line.substr(0, hash);
                trim(line);
                if (line.empty())
                    continue;
                const size_t colon = line.find(':');
                if (colon == std::string::npos)
                    continue;
                std::string key = line.substr(0, colon);
                std::string value = line.substr(colon + 1);
                trim(key);
                trim(value);
                if (value.empty())
                    continue;
                values[key] = value;
            }

            apply_int(values, "max_orders_per_second", out.circuit_breaker.max_orders_per_second);
            apply_int(values, "max_orders_per_sec", out.circuit_breaker.max_orders_per_second);
            apply_int(values, "max_orders_per_minute", out.circuit_breaker.max_orders_per_minute);
            apply_int(values, "max_orders_per_min", out.circuit_breaker.max_orders_per_minute);
            apply_int(values, "max_messages_per_second", out.circuit_breaker.max_messages_per_second);

            apply_double(values, "max_drawdown_usd", out.circuit_breaker.max_drawdown_usd);
            apply_int64(values, "max_book_age_ms", out.circuit_breaker.max_book_age_ms);
            apply_int64(values, "max_age_ms", out.circuit_breaker.max_book_age_ms);
            apply_double(values, "max_price_deviation_bps",
                         out.circuit_breaker.max_price_deviation_bps);
            apply_double(values, "price_ema_alpha", out.circuit_breaker.price_ema_alpha);
            apply_int(values, "circuit_breaker_count", out.circuit_breaker.consecutive_loss_count);
            apply_double(values, "circuit_breaker_loss_usd", out.circuit_breaker.per_leg_loss_usd);

            double heartbeat_timeout_ms = static_cast<double>(out.heartbeat_timeout_ns) / 1e6;
            apply_double(values, "heartbeat_timeout_ms", heartbeat_timeout_ms);
            apply_double(values, "timeout_ms", heartbeat_timeout_ms);
            out.heartbeat_timeout_ns = static_cast<int64_t>(heartbeat_timeout_ms * 1e6);

            apply_double(values, "target_range_usd", out.target_range_usd);
            apply_double(values, "book_target_range_usd", out.target_range_usd);
            apply_size_t(values, "max_book_levels", out.max_book_levels);

            apply_double(values, "max_gross_notional", out.global_risk.max_gross_notional);
            apply_double(values, "max_portfolio_notional", out.global_risk.max_gross_notional);
            apply_double(values, "max_net_notional", out.global_risk.max_net_notional);
            apply_double(values, "max_cross_venue_net_notional",
                         out.global_risk.max_cross_venue_net_notional);
            apply_double(values, "max_notional_per_exchange", out.global_risk.max_venue_notional);
            apply_double(values, "max_venue_notional", out.global_risk.max_venue_notional);

            double concentration_pct = out.global_risk.max_symbol_concentration * 100.0;
            apply_double(values, "max_symbol_concentration_pct", concentration_pct);
            out.global_risk.max_symbol_concentration = concentration_pct / 100.0;

            int32_t futures_risk_enabled = out.futures_risk.enabled ? 1 : 0;
            apply_int(values, "futures_risk_enabled", futures_risk_enabled);
            out.futures_risk.enabled = futures_risk_enabled != 0;
            apply_double(values, "futures_max_projected_funding_cost_bps",
                         out.futures_risk.max_projected_funding_cost_bps);
            apply_double(values, "futures_funding_cost_scale_start_bps",
                         out.futures_risk.funding_cost_scale_start_bps);
            apply_double(values, "futures_min_funding_scale", out.futures_risk.min_funding_scale);
            apply_double(values, "futures_max_mark_index_divergence_bps",
                         out.futures_risk.max_mark_index_divergence_bps);
            apply_double(values, "futures_max_maintenance_margin_ratio",
                         out.futures_risk.max_maintenance_margin_ratio);
            apply_double(values, "futures_default_max_leverage",
                         out.futures_risk.default_max_leverage);

            const std::array<const char *, 8> symbol_keys = {"BTCUSDT", "ETHUSDT", "SOLUSDT", "BNBUSDT",
                                                              "XRPUSDT", "ADAUSDT", "DOGEUSDT", "LTCUSDT"};
            out.futures_risk.symbol_limit_count = 0;
            for (const char *symbol: symbol_keys) {
                std::string key("futures_max_leverage_");
                key += symbol;
                auto it = values.find(key);
                if (it == values.end())
                    continue;
                if (out.futures_risk.symbol_limit_count >= out.futures_risk.symbol_limits.size())
                    break;
                double max_leverage = 0.0;
                if (!parse_double(it->second, max_leverage) || max_leverage <= 0.0)
                    continue;
                auto &entry = out.futures_risk.symbol_limits[out.futures_risk.symbol_limit_count++];
                std::strncpy(entry.symbol, symbol, sizeof(entry.symbol) - 1);
                entry.symbol[sizeof(entry.symbol) - 1] = '\0';
                entry.max_leverage = max_leverage;
            }

            apply_double(values, "binance_taker", out.binance_taker_fee_bps);
            apply_double(values, "BINANCE", out.binance_taker_fee_bps);
            apply_double(values, "kraken_taker", out.kraken_taker_fee_bps);
            apply_double(values, "KRAKEN", out.kraken_taker_fee_bps);
            apply_double(values, "okx_taker", out.okx_taker_fee_bps);
            apply_double(values, "OKX", out.okx_taker_fee_bps);
            apply_double(values, "coinbase_taker", out.coinbase_taker_fee_bps);
            apply_double(values, "COINBASE", out.coinbase_taker_fee_bps);

            apply_double(values, "capital_start_equity_usd_BINANCE",
                         out.venue_capital[venue_index(Exchange::BINANCE)].starting_equity_usd);
            apply_double(values, "capital_start_equity_usd_KRAKEN",
                         out.venue_capital[venue_index(Exchange::KRAKEN)].starting_equity_usd);
            apply_double(values, "capital_start_equity_usd_OKX",
                         out.venue_capital[venue_index(Exchange::OKX)].starting_equity_usd);
            apply_double(values, "capital_start_equity_usd_COINBASE",
                         out.venue_capital[venue_index(Exchange::COINBASE)].starting_equity_usd);

            apply_double(values, "capital_min_free_collateral_usd_BINANCE",
                         out.venue_capital[venue_index(Exchange::BINANCE)].min_free_collateral_buffer_usd);
            apply_double(values, "capital_min_free_collateral_usd_KRAKEN",
                         out.venue_capital[venue_index(Exchange::KRAKEN)].min_free_collateral_buffer_usd);
            apply_double(values, "capital_min_free_collateral_usd_OKX",
                         out.venue_capital[venue_index(Exchange::OKX)].min_free_collateral_buffer_usd);
            apply_double(values, "capital_min_free_collateral_usd_COINBASE",
                         out.venue_capital[venue_index(Exchange::COINBASE)].min_free_collateral_buffer_usd);

            return true;
        }

    private:
        static size_t venue_index(Exchange exchange) {
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

        static std::vector<std::string> candidate_paths(const std::string &path) {
            if (path.empty() || path[0] == '/')
                return {path};
            return {path, "../" + path, "../../" + path, "../../../" + path};
        }

        static void trim(std::string &s) {
            size_t start = 0;
            while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])))
                ++start;
            size_t end = s.size();
            while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
                --end;
            s = s.substr(start, end - start);
        }

        static bool parse_double(const std::string &s, double &out) {
            try {
                out = std::stod(s);
                return true;
            } catch (...) {
                return false;
            }
        }

        static bool parse_int(const std::string &s, int32_t &out) {
            try {
                out = static_cast<int32_t>(std::stoi(s));
                return true;
            } catch (...) {
                return false;
            }
        }

        static bool parse_int64_t(const std::string &s, int64_t &out) {
            try {
                out = static_cast<int64_t>(std::stoll(s));
                return true;
            } catch (...) {
                return false;
            }
        }

        static void apply_double(const std::unordered_map<std::string, std::string> &values,
                                 const char *key, double &field) {
            auto it = values.find(key);
            if (it == values.end())
                return;
            double v = 0.0;
            if (parse_double(it->second, v))
                field = v;
        }

        static void apply_size_t(const std::unordered_map<std::string, std::string> &values,
                                 const char *key, size_t &field) {
            auto it = values.find(key);
            if (it == values.end())
                return;
            try {
                const size_t v = std::stoull(it->second);
                if (v > 0)
                    field = v;
            } catch (...) {
            }
        }

        static void apply_int(const std::unordered_map<std::string, std::string> &values,
                              const char *key, int32_t &field) {
            auto it = values.find(key);
            if (it == values.end())
                return;
            int32_t v = 0;
            if (parse_int(it->second, v))
                field = v;
        }

        static void apply_int64(const std::unordered_map<std::string, std::string> &values,
                                const char *key, int64_t &field) {
            auto it = values.find(key);
            if (it == values.end())
                return;
            int64_t v = 0;
            if (parse_int64_t(it->second, v))
                field = v;
        }
    };
} 
