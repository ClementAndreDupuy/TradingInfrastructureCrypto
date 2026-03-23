#pragma once

#include "../../../common/types.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>

namespace trading {
    struct PositionLedgerVenueSnapshot {
        Exchange exchange = Exchange::UNKNOWN;
        char symbol[16] = {};
        double position = 0.0;
        double avg_entry_price = 0.0;
        double realized_pnl = 0.0;
        double unrealized_pnl = 0.0;
        double pending_bid_qty = 0.0;
        double pending_ask_qty = 0.0;
        double mid_price = 0.0;
        int64_t inventory_age_ms = 0;
        bool active = false;
    };

    struct PositionLedgerSnapshot {
        char symbol[16] = {};
        double global_position = 0.0;
        double global_avg_entry_price = 0.0;
        double realized_pnl = 0.0;
        double unrealized_pnl = 0.0;
        double pending_bid_qty = 0.0;
        double pending_ask_qty = 0.0;
        double mid_price = 0.0;
        int64_t oldest_inventory_age_ms = 0;
        size_t venue_count = 0;
        std::array<PositionLedgerVenueSnapshot, 4> venues{};
    };

    class PositionLedger {
    public:
        static constexpr size_t MAX_VENUES = 4;

        void on_order_submitted(const Order &order) noexcept {
            auto *venue = find_or_create_venue(order.exchange, order.symbol);
            if (!venue)
                return;
            if (order.side == Side::BID)
                venue->pending_bid_qty += order.quantity;
            else
                venue->pending_ask_qty += order.quantity;
            copy_symbol(symbol_, order.symbol);
        }

        void on_order_closed(const Order &order, double remaining_qty) noexcept {
            auto *venue = find_venue(order.exchange, order.symbol);
            if (!venue)
                return;
            const double qty = std::max(0.0, remaining_qty);
            if (order.side == Side::BID)
                venue->pending_bid_qty = std::max(0.0, venue->pending_bid_qty - qty);
            else
                venue->pending_ask_qty = std::max(0.0, venue->pending_ask_qty - qty);
        }

        void on_fill(const Order &order, const FillUpdate &fill) noexcept {
            auto *venue = find_or_create_venue(order.exchange, order.symbol);
            if (!venue)
                return;

            copy_symbol(symbol_, order.symbol);
            if (order.side == Side::BID)
                venue->pending_bid_qty = std::max(0.0, venue->pending_bid_qty - fill.fill_qty);
            else
                venue->pending_ask_qty = std::max(0.0, venue->pending_ask_qty - fill.fill_qty);

            apply_inventory_fill(*venue, order.side, fill.fill_qty, fill.fill_price);
        }

        void update_mid_price(const char *symbol, Exchange exchange, double mid_price) noexcept {
            auto *venue = find_or_create_venue(exchange, symbol);
            if (!venue)
                return;
            venue->mid_price = mid_price;
            copy_symbol(symbol_, symbol);
        }

        PositionLedgerSnapshot snapshot() const noexcept {
            PositionLedgerSnapshot out;
            copy_symbol(out.symbol, symbol_);

            for (const auto &venue: venues_) {
                if (!venue.active)
                    continue;

                out.venues[out.venue_count++] = build_venue_snapshot(venue);
                out.global_position += venue.position;
                out.realized_pnl += venue.realized_pnl;
                out.unrealized_pnl += compute_unrealized_pnl(venue);
                out.pending_bid_qty += venue.pending_bid_qty;
                out.pending_ask_qty += venue.pending_ask_qty;
                if (venue.mid_price > 0.0)
                    out.mid_price = venue.mid_price;
                if (venue.position != 0.0)
                    out.oldest_inventory_age_ms = std::max(out.oldest_inventory_age_ms,
                                                           inventory_age_ms(venue));
            }

            out.global_avg_entry_price = compute_weighted_average_entry_price();
            return out;
        }

    private:
        struct VenuePosition {
            Exchange exchange = Exchange::UNKNOWN;
            char symbol[16] = {};
            double position = 0.0;
            double avg_entry_price = 0.0;
            double realized_pnl = 0.0;
            double pending_bid_qty = 0.0;
            double pending_ask_qty = 0.0;
            double mid_price = 0.0;
            std::chrono::steady_clock::time_point opened_at{};
            bool has_inventory_age = false;
            bool active = false;
        };

        std::array<VenuePosition, MAX_VENUES> venues_{};
        char symbol_[16] = {};

        static void copy_symbol(char (&dst)[16], const char *src) noexcept {
            if (!src) {
                dst[0] = '\0';
                return;
            }
            std::strncpy(dst, src, sizeof(dst) - 1);
            dst[sizeof(dst) - 1] = '\0';
        }

        VenuePosition *find_venue(Exchange exchange, const char *symbol) noexcept {
            for (auto &venue: venues_) {
                if (venue.active && venue.exchange == exchange &&
                    std::strncmp(venue.symbol, symbol, sizeof(venue.symbol)) == 0)
                    return &venue;
            }
            return nullptr;
        }

        VenuePosition *find_or_create_venue(Exchange exchange, const char *symbol) noexcept {
            if (auto *existing = find_venue(exchange, symbol))
                return existing;

            for (auto &venue: venues_) {
                if (venue.active)
                    continue;
                venue = {};
                venue.exchange = exchange;
                venue.active = true;
                copy_symbol(venue.symbol, symbol);
                return &venue;
            }
            return nullptr;
        }

        static int position_sign(double value) noexcept {
            return (value > 0.0) - (value < 0.0);
        }

        static int64_t inventory_age_ms(const VenuePosition &venue) noexcept {
            if (!venue.has_inventory_age || venue.position == 0.0)
                return 0;
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now() - venue.opened_at).count();
        }

        static double compute_unrealized_pnl(const VenuePosition &venue) noexcept {
            if (venue.position == 0.0 || venue.avg_entry_price <= 0.0 || venue.mid_price <= 0.0)
                return 0.0;
            if (venue.position > 0.0)
                return (venue.mid_price - venue.avg_entry_price) * venue.position;
            return (venue.avg_entry_price - venue.mid_price) * (-venue.position);
        }

        PositionLedgerVenueSnapshot build_venue_snapshot(const VenuePosition &venue) const noexcept {
            PositionLedgerVenueSnapshot out;
            out.exchange = venue.exchange;
            copy_symbol(out.symbol, venue.symbol);
            out.position = venue.position;
            out.avg_entry_price = venue.avg_entry_price;
            out.realized_pnl = venue.realized_pnl;
            out.unrealized_pnl = compute_unrealized_pnl(venue);
            out.pending_bid_qty = venue.pending_bid_qty;
            out.pending_ask_qty = venue.pending_ask_qty;
            out.mid_price = venue.mid_price;
            out.inventory_age_ms = inventory_age_ms(venue);
            out.active = true;
            return out;
        }

        double compute_weighted_average_entry_price() const noexcept {
            double weighted_notional = 0.0;
            double total_qty = 0.0;
            int net_sign = 0;
            for (const auto &venue: venues_) {
                if (!venue.active || venue.position == 0.0 || venue.avg_entry_price <= 0.0)
                    continue;
                const int sign = position_sign(venue.position);
                if (net_sign == 0)
                    net_sign = sign;
                if (sign != net_sign)
                    return 0.0;
                weighted_notional += std::abs(venue.position) * venue.avg_entry_price;
                total_qty += std::abs(venue.position);
            }
            if (total_qty == 0.0)
                return 0.0;
            return weighted_notional / total_qty;
        }

        static void apply_inventory_fill(VenuePosition &venue, Side side, double qty,
                                         double price) noexcept {
            if (qty <= 0.0 || price <= 0.0)
                return;

            const double signed_qty = side == Side::BID ? qty : -qty;
            const double prior_position = venue.position;
            const int prior_sign = position_sign(prior_position);
            const int fill_sign = position_sign(signed_qty);
            const double abs_prior = std::abs(prior_position);
            const double abs_fill = std::abs(signed_qty);

            if (prior_sign == 0 || prior_sign == fill_sign) {
                const double new_position = prior_position + signed_qty;
                const double total_qty = abs_prior + abs_fill;
                venue.avg_entry_price = total_qty > 0.0
                                            ? ((venue.avg_entry_price * abs_prior) + (price * abs_fill)) /
                                              total_qty
                                            : 0.0;
                venue.position = new_position;
                if (prior_position == 0.0 && new_position != 0.0) {
                    venue.opened_at = std::chrono::steady_clock::now();
                    venue.has_inventory_age = true;
                }
                if (new_position == 0.0) {
                    venue.avg_entry_price = 0.0;
                    venue.has_inventory_age = false;
                }
                return;
            }

            const double closing_qty = std::min(abs_prior, abs_fill);
            if (prior_position > 0.0)
                venue.realized_pnl += (price - venue.avg_entry_price) * closing_qty;
            else
                venue.realized_pnl += (venue.avg_entry_price - price) * closing_qty;

            const double new_position = prior_position + signed_qty;
            if (new_position == 0.0) {
                venue.position = 0.0;
                venue.avg_entry_price = 0.0;
                venue.has_inventory_age = false;
                return;
            }

            const int new_sign = position_sign(new_position);
            venue.position = new_position;
            if (new_sign != prior_sign) {
                venue.avg_entry_price = price;
                venue.opened_at = std::chrono::steady_clock::now();
                venue.has_inventory_age = true;
            }
        }
    };
}
