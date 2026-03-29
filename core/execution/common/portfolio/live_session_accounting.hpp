#pragma once

#include "../../../common/types.hpp"
#include "../reconciliation/reconciliation_types.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace trading {
    struct LiveSessionVenueMetrics {
        Exchange exchange = Exchange::UNKNOWN;
        double start_equity = 0.0;
        double end_equity = 0.0;
        double realized_pnl = 0.0;
        double unrealized_pnl = 0.0;
        double fees = 0.0;
        double net_pnl = 0.0;
        double return_pct = 0.0;
        double free_collateral = 0.0;
        double min_free_collateral_buffer = 0.0;
        bool has_reconciled_equity = false;
    };

    struct LiveSessionGlobalMetrics {
        double start_equity = 0.0;
        double end_equity = 0.0;
        double realized_pnl = 0.0;
        double unrealized_pnl = 0.0;
        double fees = 0.0;
        double net_pnl = 0.0;
        double return_pct = 0.0;
    };

    class LiveSessionAccounting {
    public:
        static constexpr size_t MAX_VENUES = 4;
        static constexpr size_t MAX_SEEN_FILLS_PER_VENUE = 4096;

        LiveSessionAccounting() {
            for (size_t i = 0; i < venues_.size(); ++i) {
                venues_[i].metrics.exchange = index_to_exchange(i);
            }
        }

        void configure_venue(Exchange exchange, double start_equity,
                             double min_free_collateral_buffer) noexcept {
            VenueState &state = venues_[venue_index(exchange)];
            state.metrics.start_equity = std::max(0.0, start_equity);
            state.metrics.min_free_collateral_buffer = std::max(0.0, min_free_collateral_buffer);
        }

        void ingest_reconciliation(Exchange exchange, const ReconciliationSnapshot &snapshot,
                                   const char *symbol, double mark_price) noexcept {
            VenueState &state = venues_[venue_index(exchange)];
            state.metrics.free_collateral = sum_available(snapshot);
            state.metrics.end_equity = sum_total(snapshot);
            state.metrics.has_reconciled_equity = true;
            state.metrics.unrealized_pnl = compute_unrealized(snapshot, symbol, mark_price);
            state.metrics.fees += collect_new_fees(state, snapshot);
            update_derived(state.metrics);
            last_update_ts_ns_ = now_ns();
        }

        bool can_afford_spot_order(Exchange exchange, const char *symbol, Side side, double quantity,
                                   double price, double fee_bps) const noexcept {
            if (!std::isfinite(quantity) || !std::isfinite(price) || quantity <= 0.0 || price <= 0.0)
                return false;

            const VenueState &state = venues_[venue_index(exchange)];
            const double required_buffer = state.metrics.min_free_collateral_buffer;
            const std::string_view quote_asset = infer_quote_asset(symbol);
            const std::string_view base_asset = infer_base_asset(symbol, quote_asset);

            if (side == Side::BID) {
                const double buy_notional = quantity * price;
                const double fee = buy_notional * std::max(0.0, fee_bps) / 10000.0;
                double available_quote = find_available_asset(state.last_snapshot, quote_asset);
                if (available_quote <= 0.0 && is_stable_quote(quote_asset))
                    available_quote = state.metrics.start_equity;
                return available_quote >= buy_notional + fee + required_buffer;
            }

            const double available_base = find_available_asset(state.last_snapshot, base_asset);
            return available_base >= quantity;
        }

        bool can_afford_futures_order(Exchange exchange, double quantity, double price,
                                      double fee_bps, double leverage) const noexcept {
            if (!std::isfinite(quantity) || !std::isfinite(price) || quantity <= 0.0 || price <= 0.0)
                return false;

            const VenueState &state = venues_[venue_index(exchange)];
            const double applied_leverage = std::max(1.0, leverage);
            const double notional = quantity * price;
            const double initial_margin = notional / applied_leverage;
            const double fee = notional * std::max(0.0, fee_bps) / 10000.0;
            const double required = initial_margin + fee + state.metrics.min_free_collateral_buffer;
            const double available = state.metrics.free_collateral > 0.0
                                         ? state.metrics.free_collateral
                                         : state.metrics.start_equity;
            return available >= required;
        }

        LiveSessionVenueMetrics venue_metrics(Exchange exchange) const noexcept {
            return venues_[venue_index(exchange)].metrics;
        }

        LiveSessionGlobalMetrics global_metrics() const noexcept {
            LiveSessionGlobalMetrics out;
            for (const auto &state: venues_) {
                out.start_equity += state.metrics.start_equity;
                out.end_equity += state.metrics.end_equity;
                out.unrealized_pnl += state.metrics.unrealized_pnl;
                out.fees += state.metrics.fees;
            }
            out.net_pnl = out.end_equity - out.start_equity;
            out.realized_pnl = out.net_pnl - out.unrealized_pnl + out.fees;
            if (out.start_equity > 0.0)
                out.return_pct = (out.net_pnl / out.start_equity) * 100.0;
            return out;
        }

        int64_t last_update_ts_ns() const noexcept { return last_update_ts_ns_; }

    private:
        struct SeenFillKey {
            char venue_trade_id[64] = {};
            int64_t exchange_ts_ns = 0;
            bool active = false;
        };

        struct VenueState {
            LiveSessionVenueMetrics metrics;
            ReconciliationSnapshot last_snapshot;
            std::array<SeenFillKey, MAX_SEEN_FILLS_PER_VENUE> seen_fill_keys{};
            size_t seen_fill_cursor = 0;
        };

        std::array<VenueState, MAX_VENUES> venues_{};
        int64_t last_update_ts_ns_ = 0;

        static size_t venue_index(Exchange exchange) noexcept {
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

        static Exchange index_to_exchange(size_t idx) noexcept {
            switch (idx) {
                case 0:
                    return Exchange::BINANCE;
                case 1:
                    return Exchange::OKX;
                case 2:
                    return Exchange::COINBASE;
                case 3:
                    return Exchange::KRAKEN;
                default:
                    return Exchange::UNKNOWN;
            }
        }

        static int64_t now_ns() noexcept {
            using namespace std::chrono;
            return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
        }

        static double sum_total(const ReconciliationSnapshot &snapshot) noexcept {
            double total = 0.0;
            for (size_t i = 0; i < snapshot.balances.size; ++i)
                total += std::max(0.0, snapshot.balances.items[i].total);
            return total;
        }

        static double sum_available(const ReconciliationSnapshot &snapshot) noexcept {
            double total = 0.0;
            for (size_t i = 0; i < snapshot.balances.size; ++i)
                total += std::max(0.0, snapshot.balances.items[i].available);
            return total;
        }

        static bool is_stable_quote(std::string_view asset) noexcept {
            return asset == "USDT" || asset == "USD" || asset == "USDC";
        }

        static std::string_view infer_quote_asset(const char *symbol) noexcept {
            const std::string_view s = symbol ? std::string_view(symbol) : std::string_view{};
            if (s.size() >= 4 && s.substr(s.size() - 4) == "USDT")
                return s.substr(s.size() - 4);
            if (s.size() >= 4 && s.substr(s.size() - 4) == "USDC")
                return s.substr(s.size() - 4);
            if (s.size() >= 3 && s.substr(s.size() - 3) == "USD")
                return s.substr(s.size() - 3);
            return {};
        }

        static std::string_view infer_base_asset(const char *symbol,
                                                 std::string_view quote_asset) noexcept {
            const std::string_view s = symbol ? std::string_view(symbol) : std::string_view{};
            if (quote_asset.empty() || s.size() <= quote_asset.size())
                return s;
            return s.substr(0, s.size() - quote_asset.size());
        }

        static double find_available_asset(const ReconciliationSnapshot &snapshot,
                                           std::string_view asset) noexcept {
            if (asset.empty())
                return 0.0;
            for (size_t i = 0; i < snapshot.balances.size; ++i) {
                const auto &balance = snapshot.balances.items[i];
                if (std::string_view(balance.asset) == asset)
                    return std::max(0.0, balance.available);
            }
            return 0.0;
        }

        static double compute_unrealized(const ReconciliationSnapshot &snapshot, const char *symbol,
                                         double mark_price) noexcept {
            if (!std::isfinite(mark_price) || mark_price <= 0.0)
                return 0.0;
            double unrealized = 0.0;
            for (size_t i = 0; i < snapshot.positions.size; ++i) {
                const auto &position = snapshot.positions.items[i];
                if (symbol && symbol[0] != '\0' && std::string_view(position.symbol) != std::string_view(symbol))
                    continue;
                if (!std::isfinite(position.quantity) || !std::isfinite(position.avg_entry_price) ||
                    position.avg_entry_price <= 0.0)
                    continue;
                if (position.quantity > 0.0)
                    unrealized += (mark_price - position.avg_entry_price) * position.quantity;
                else
                    unrealized += (position.avg_entry_price - mark_price) * (-position.quantity);
            }
            return unrealized;
        }

        static bool is_fill_seen(const VenueState &state, const ReconciledFill &fill) noexcept {
            for (const auto &key: state.seen_fill_keys) {
                if (!key.active)
                    continue;
                if (key.exchange_ts_ns != fill.exchange_ts_ns)
                    continue;
                if (std::strncmp(key.venue_trade_id, fill.venue_trade_id, sizeof(key.venue_trade_id)) == 0)
                    return true;
            }
            return false;
        }

        static void remember_fill(VenueState &state, const ReconciledFill &fill) noexcept {
            if (state.seen_fill_keys.empty())
                return;
            SeenFillKey &slot = state.seen_fill_keys[state.seen_fill_cursor % state.seen_fill_keys.size()];
            std::strncpy(slot.venue_trade_id, fill.venue_trade_id, sizeof(slot.venue_trade_id) - 1);
            slot.venue_trade_id[sizeof(slot.venue_trade_id) - 1] = '\0';
            slot.exchange_ts_ns = fill.exchange_ts_ns;
            slot.active = true;
            ++state.seen_fill_cursor;
        }

        static double fee_to_usd(const ReconciledFill &fill) noexcept {
            if (!std::isfinite(fill.fee) || fill.fee <= 0.0)
                return 0.0;
            const std::string_view fee_asset(fill.fee_asset);
            if (is_stable_quote(fee_asset))
                return fill.fee;
            if (std::string_view(fill.symbol).size() >= fee_asset.size() && !fee_asset.empty())
                return fill.fee * std::max(0.0, fill.price);
            return fill.fee;
        }

        static double collect_new_fees(VenueState &state, const ReconciliationSnapshot &snapshot) noexcept {
            double fees = 0.0;
            for (size_t i = 0; i < snapshot.fills.size; ++i) {
                const auto &fill = snapshot.fills.items[i];
                if (is_fill_seen(state, fill))
                    continue;
                fees += fee_to_usd(fill);
                remember_fill(state, fill);
            }
            state.last_snapshot = snapshot;
            return fees;
        }

        static void update_derived(LiveSessionVenueMetrics &metrics) noexcept {
            metrics.net_pnl = metrics.end_equity - metrics.start_equity;
            metrics.realized_pnl = metrics.net_pnl - metrics.unrealized_pnl + metrics.fees;
            if (metrics.start_equity > 0.0)
                metrics.return_pct = (metrics.net_pnl / metrics.start_equity) * 100.0;
            else
                metrics.return_pct = 0.0;
        }
    };
}
