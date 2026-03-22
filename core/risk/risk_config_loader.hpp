#pragma once

#include "circuit_breaker.hpp"
#include "global_risk_controls.hpp"

#include <cctype>
#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace trading {
    struct RiskRuntimeConfig {
        CircuitBreakerConfig circuit_breaker;
        GlobalRiskConfig global_risk;
        int64_t heartbeat_timeout_ns = KillSwitch::DEFAULT_HEARTBEAT_TIMEOUT_NS;
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

            return true;
        }

    private:
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
} // namespace trading
