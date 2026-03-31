#pragma once

#include "../execution/common/orders/child_order_scheduler.hpp"
#include "../execution/common/portfolio/portfolio_intent_engine.hpp"
#include "../execution/common/quality/venue_quality_model.hpp"
#include "../execution/router/smart_order_router.hpp"
#include "../shadow/shadow_engine.hpp"

#include <cctype>
#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace trading {
    struct VenueQuoteDefaults {
        double latency_penalty_bps;
        double risk_penalty_bps;
        double fill_probability;
        double queue_ahead_qty;
        double toxicity_bps;
    };

    struct FuturesLeverageCap {
        std::string symbol;
        double max_leverage;
    };

    struct BinanceFuturesRuntimeConfig {
        bool enabled;
        std::string rest_url;
        uint32_t recv_window_ms;
        bool hedge_mode;
        double default_leverage_cap;
        std::vector<FuturesLeverageCap> leverage_caps;
    };

    enum class StrategyMode : uint8_t {
        UNKNOWN = 0,
        SPOT_ONLY = 1,
        FUTURES_ONLY = 2,
    };

    struct EngineConfig {
        int reconnect_interval_secs;
        int reconciliation_interval_secs;
        int portfolio_log_heartbeat_secs;
        int venue_quality_log_heartbeat_secs;
        VenueQuoteDefaults venue_quote_defaults;
        std::string binance_rest_url;
        std::string binance_ws_url;
        std::string kraken_rest_url;
        std::string kraken_ws_url;
        std::string okx_rest_url;
        std::string okx_ws_url;
        std::string coinbase_rest_url;
        std::string coinbase_ws_url;
        int64_t shadow_base_latency_ns;
        int64_t shadow_latency_jitter_ns;
        double shadow_impact_slippage_per_notional_bps;
        double shadow_queue_match_fraction_per_check;
        std::string shadow_log_path;
        int retry_max_attempts;
        int retry_backoff_ms;
        StrategyMode strategy_mode = StrategyMode::UNKNOWN;
        BinanceFuturesRuntimeConfig binance_futures;
    };

    class AlgoConfigLoader {
    public:
        static bool load_routing(const std::string &path, SmartOrderRouterConfig &sor_out,
                                 ChildOrderScheduler::Config &sched_out,
                                 RoutingConstraints &constraints_out) {
            const auto values = parse_file(path);
            if (values.empty())
                return false;

            apply_double(values, "high_toxicity_threshold", sor_out.high_toxicity_threshold);
            apply_double(values, "low_fill_threshold", sor_out.low_fill_threshold);
            apply_double(values, "high_fill_threshold", sor_out.high_fill_threshold);
            apply_double(values, "high_toxicity_fill_weight", sor_out.high_toxicity_fill_weight);
            apply_double(values, "high_toxicity_queue_weight", sor_out.high_toxicity_queue_weight);
            apply_double(values, "high_toxicity_tox_weight", sor_out.high_toxicity_tox_weight);
            apply_double(values, "low_fill_fill_weight", sor_out.low_fill_fill_weight);
            apply_double(values, "low_fill_queue_weight", sor_out.low_fill_queue_weight);
            apply_double(values, "low_fill_tox_weight", sor_out.low_fill_tox_weight);
            apply_double(values, "high_fill_fill_weight", sor_out.high_fill_fill_weight);
            apply_double(values, "high_fill_queue_weight", sor_out.high_fill_queue_weight);
            apply_double(values, "high_fill_tox_weight", sor_out.high_fill_tox_weight);
            apply_double(values, "min_fill_probability", sor_out.min_fill_probability);

            apply_int64(values, "short_horizon_ticks", sched_out.short_horizon_ticks);
            apply_int64(values, "sweep_horizon_ticks", sched_out.sweep_horizon_ticks);
            apply_int64(values, "long_horizon_ticks", sched_out.long_horizon_ticks);
            apply_double(values, "passive_join_inventory_age_bps",
                         sched_out.passive_join_inventory_age_bps);
            apply_double(values, "passive_improve_inventory_age_bps",
                         sched_out.passive_improve_inventory_age_bps);
            apply_double(values, "aggressive_inventory_age_bps",
                         sched_out.aggressive_inventory_age_bps);
            apply_double(values, "passive_improve_fraction", sched_out.passive_improve_fraction);

            apply_double(values, "alpha_min_signal_bps", constraints_out.alpha_min_signal_bps);
            apply_double(values, "alpha_risk_max", constraints_out.alpha_risk_max);
            apply_double(values, "alpha_qty_scale", constraints_out.alpha_qty_scale);

            sched_out.sor = sor_out;
            return true;
        }

        static bool load_portfolio(const std::string &path, PortfolioIntentConfig &out) {
            const auto values = parse_file(path);
            if (values.empty())
                return false;

            apply_double(values, "max_position", out.max_position);
            apply_double(values, "min_entry_signal_bps", out.min_entry_signal_bps);
            apply_double(values, "alpha_exit_buffer_bps", out.alpha_exit_buffer_bps);
            apply_double(values, "negative_reversal_signal_bps", out.negative_reversal_signal_bps);
            apply_double(values, "positive_reversal_signal_bps", out.positive_reversal_signal_bps);
            apply_double(values, "deadband_signal_bps", out.deadband_signal_bps);
            apply_double(values, "max_risk_score", out.max_risk_score);
            apply_double(values, "shock_enter_threshold", out.shock_enter_threshold);
            apply_double(values, "shock_exit_threshold", out.shock_exit_threshold);
            apply_double(values, "illiquid_enter_threshold", out.illiquid_enter_threshold);
            apply_double(values, "illiquid_exit_threshold", out.illiquid_exit_threshold);
            apply_int(values, "regime_persistence_ticks", out.regime_persistence_ticks);
            apply_int64(values, "stale_inventory_ms", out.stale_inventory_ms);
            apply_int64(values, "stale_signal_ms", out.stale_signal_ms);
            apply_double(values, "max_basis_divergence_bps", out.max_basis_divergence_bps);
            apply_double(values, "stale_inventory_alpha_hold_bps",
                         out.stale_inventory_alpha_hold_bps);
            apply_double(values, "health_reduce_ratio", out.health_reduce_ratio);
            apply_bool(values, "long_only", out.long_only);
            return true;
        }

        static bool load_venue_quality(const std::string &path, VenueQualityModel::Config &out) {
            const auto values = parse_file(path);
            if (values.empty())
                return false;

            apply_double(values, "min_fill_probability", out.min_fill_probability);
            apply_double(values, "max_fill_probability", out.max_fill_probability);
            apply_double(values, "fill_probability_alpha", out.fill_probability_alpha);
            apply_double(values, "markout_alpha", out.markout_alpha);
            apply_double(values, "reject_rate_alpha", out.reject_rate_alpha);
            apply_double(values, "cancel_latency_alpha", out.cancel_latency_alpha);
            apply_double(values, "health_alpha", out.health_alpha);
            apply_double(values, "max_fill_step", out.max_fill_step);
            apply_double(values, "max_markout_step_bps", out.max_markout_step_bps);
            apply_double(values, "max_reject_step", out.max_reject_step);
            apply_double(values, "max_cancel_latency_step_bps", out.max_cancel_latency_step_bps);
            apply_double(values, "max_health_step_bps", out.max_health_step_bps);
            apply_double(values, "reject_rate_penalty_bps", out.reject_rate_penalty_bps);
            apply_double(values, "cancel_latency_penalty_scale_bps",
                         out.cancel_latency_penalty_scale_bps);
            apply_double(values, "health_penalty_scale_bps", out.health_penalty_scale_bps);
            apply_double(values, "passive_markout_penalty_scale", out.passive_markout_penalty_scale);
            apply_double(values, "taker_markout_penalty_scale", out.taker_markout_penalty_scale);
            apply_double(values, "stability_penalty_scale", out.stability_penalty_scale);
            return true;
        }

        static bool load_engine(const std::string &path, EngineConfig &out) {
            const auto values = parse_file(path);
            if (values.empty())
                return false;

            apply_int(values, "reconnect_interval_secs", out.reconnect_interval_secs);
            apply_int(values, "reconciliation_interval_secs", out.reconciliation_interval_secs);
            apply_int(values, "portfolio_log_heartbeat_secs", out.portfolio_log_heartbeat_secs);
            apply_int(values, "venue_quality_log_heartbeat_secs",
                      out.venue_quality_log_heartbeat_secs);

            apply_double(values, "default_latency_penalty_bps",
                         out.venue_quote_defaults.latency_penalty_bps);
            apply_double(values, "default_risk_penalty_bps",
                         out.venue_quote_defaults.risk_penalty_bps);
            apply_double(values, "default_fill_probability",
                         out.venue_quote_defaults.fill_probability);
            apply_double(values, "default_queue_ahead_qty",
                         out.venue_quote_defaults.queue_ahead_qty);
            apply_double(values, "default_toxicity_bps", out.venue_quote_defaults.toxicity_bps);

            apply_string(values, "binance_rest_url", out.binance_rest_url);
            apply_string(values, "binance_ws_url", out.binance_ws_url);
            apply_string(values, "kraken_rest_url", out.kraken_rest_url);
            apply_string(values, "kraken_ws_url", out.kraken_ws_url);
            apply_string(values, "okx_rest_url", out.okx_rest_url);
            apply_string(values, "okx_ws_url", out.okx_ws_url);
            apply_string(values, "coinbase_rest_url", out.coinbase_rest_url);
            apply_string(values, "coinbase_ws_url", out.coinbase_ws_url);

            apply_int64(values, "shadow_base_latency_ns", out.shadow_base_latency_ns);
            apply_int64(values, "shadow_latency_jitter_ns", out.shadow_latency_jitter_ns);
            apply_double(values, "shadow_impact_slippage_per_notional_bps",
                         out.shadow_impact_slippage_per_notional_bps);
            apply_double(values, "shadow_queue_match_fraction_per_check",
                         out.shadow_queue_match_fraction_per_check);
            apply_string(values, "shadow_log_path", out.shadow_log_path);
            apply_int(values, "retry_max_attempts", out.retry_max_attempts);
            apply_int(values, "retry_backoff_ms", out.retry_backoff_ms);
            apply_strategy_mode(values, "strategy_mode", out.strategy_mode);
            apply_bool(values, "binance_futures_enabled", out.binance_futures.enabled);
            apply_string(values, "binance_futures_rest_url", out.binance_futures.rest_url);
            apply_uint32(values, "binance_futures_recv_window_ms",
                         out.binance_futures.recv_window_ms);
            apply_bool(values, "binance_futures_hedge_mode", out.binance_futures.hedge_mode);
            apply_double(values, "binance_futures_default_leverage_cap",
                         out.binance_futures.default_leverage_cap);
            out.binance_futures.leverage_caps.clear();
            for (const auto &entry: values) {
                static constexpr std::string_view prefix = "binance_futures_leverage_cap_";
                if (entry.first.rfind(std::string(prefix), 0) != 0)
                    continue;
                try {
                    const double max_leverage = std::stod(entry.second);
                    if (max_leverage <= 0.0)
                        continue;
                    const std::string symbol = entry.first.substr(prefix.size());
                    if (symbol.empty())
                        continue;
                    out.binance_futures.leverage_caps.push_back({symbol, max_leverage});
                } catch (...) {
                }
            }
            return true;
        }

    private:
        using ValueMap = std::unordered_map<std::string, std::string>;

        static ValueMap parse_file(const std::string &path) {
            std::ifstream in;
            for (const std::string &candidate: candidate_paths(path)) {
                in.open(candidate);
                if (in.is_open())
                    break;
                in.clear();
            }
            if (!in.is_open())
                return {};

            ValueMap values;
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
            return values;
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

        static void apply_double(const ValueMap &values, const char *key, double &field) {
            auto it = values.find(key);
            if (it == values.end())
                return;
            try {
                field = std::stod(it->second);
            } catch (...) {
            }
        }

        static void apply_int(const ValueMap &values, const char *key, int &field) {
            auto it = values.find(key);
            if (it == values.end())
                return;
            try {
                field = std::stoi(it->second);
            } catch (...) {
            }
        }

        static void apply_int64(const ValueMap &values, const char *key, int64_t &field) {
            auto it = values.find(key);
            if (it == values.end())
                return;
            try {
                field = static_cast<int64_t>(std::stoll(it->second));
            } catch (...) {
            }
        }

        static void apply_uint32(const ValueMap &values, const char *key, uint32_t &field) {
            auto it = values.find(key);
            if (it == values.end())
                return;
            try {
                const auto parsed = std::stoul(it->second);
                field = static_cast<uint32_t>(parsed);
            } catch (...) {
            }
        }

        static void apply_strategy_mode(const ValueMap &values, const char *key, StrategyMode &field) {
            auto it = values.find(key);
            if (it == values.end())
                return;
            const std::string value = to_lower(it->second);
            if (value == "spot_only") {
                field = StrategyMode::SPOT_ONLY;
                return;
            }
            if (value == "futures_only") {
                field = StrategyMode::FUTURES_ONLY;
                return;
            }
            field = StrategyMode::UNKNOWN;
        }

        static std::string to_lower(std::string value) {
            for (char &ch: value)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return value;
        }

        static void apply_bool(const ValueMap &values, const char *key, bool &field) {
            auto it = values.find(key);
            if (it == values.end())
                return;
            field = (it->second == "true" || it->second == "1" || it->second == "yes");
        }

        static void apply_string(const ValueMap &values, const char *key, std::string &field) {
            auto it = values.find(key);
            if (it == values.end())
                return;
            if (!it->second.empty())
                field = it->second;
        }
    };
}
