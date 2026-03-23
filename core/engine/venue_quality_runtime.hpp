#pragma once

#include "../execution/common/venue_quality_model.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace trading::engine {
    class VenueQualityRuntime {
    public:
        static constexpr size_t MAX_TRACKED_ORDERS = 256;

        explicit VenueQualityRuntime(VenueQualityModel &model) : model_(model) {
        }

        void on_submit(const Order &order, ConnectorResult result,
                       std::chrono::steady_clock::time_point now) noexcept {
            if (result != ConnectorResult::OK) {
                model_.observe_reject(order.exchange, true);
                release(order.client_order_id, order.exchange);
                return;
            }
            model_.observe_reject(order.exchange, false);
            TrackedOrder *slot = find(order.client_order_id, order.exchange);
            if (slot == nullptr) {
                slot = acquire();
            }
            if (slot == nullptr) {
                return;
            }
            slot->active = true;
            slot->client_order_id = order.client_order_id;
            slot->exchange = order.exchange;
            slot->side = order.side;
            slot->tif = order.tif;
            slot->quantity = std::max(0.0, order.quantity);
            slot->filled_qty = 0.0;
            slot->submitted_at = now;
        }

        template <typename MidPriceLookup>
        void on_fill(Exchange exchange, const FillUpdate &update,
                     std::chrono::steady_clock::time_point now,
                     MidPriceLookup &&mid_price_lookup) noexcept {
            TrackedOrder *tracked = find(update.client_order_id, exchange);
            if (update.new_state == OrderState::REJECTED) {
                model_.observe_reject(exchange, true);
                release(update.client_order_id, exchange);
                return;
            }
            if (tracked == nullptr) {
                return;
            }
            if (update.fill_qty > 0.0) {
                tracked->filled_qty = std::clamp(
                    std::max(tracked->filled_qty,
                             std::max(update.cumulative_filled_qty, tracked->filled_qty + update.fill_qty)),
                    0.0, tracked->quantity);
                if (tracked->quantity > 0.0) {
                    model_.observe_fill_probability(exchange,
                                                    std::clamp(tracked->filled_qty / tracked->quantity,
                                                               0.0, 1.0));
                }
                const double mid_price = mid_price_lookup(exchange);
                if (mid_price > 0.0 && update.fill_price > 0.0) {
                    model_.observe_markout(exchange, tracked->tif == TimeInForce::GTX,
                                           realized_fill_quality_bps(*tracked, update.fill_price,
                                                                     mid_price));
                }
            }
            if (update.new_state == OrderState::CANCELED) {
                model_.observe_cancel_latency(exchange,
                                              std::chrono::duration_cast<std::chrono::microseconds>(
                                                  now - tracked->submitted_at));
                if (tracked->quantity > 0.0) {
                    model_.observe_fill_probability(exchange,
                                                    std::clamp(tracked->filled_qty / tracked->quantity,
                                                               0.0, 1.0));
                }
                release(update.client_order_id, exchange);
                return;
            }
            if (update.new_state == OrderState::FILLED) {
                if (tracked->quantity > 0.0) {
                    model_.observe_fill_probability(exchange, 1.0);
                }
                release(update.client_order_id, exchange);
            }
        }

    private:
        struct TrackedOrder {
            bool active = false;
            uint64_t client_order_id = 0;
            Exchange exchange = Exchange::UNKNOWN;
            Side side = Side::BID;
            TimeInForce tif = TimeInForce::GTC;
            double quantity = 0.0;
            double filled_qty = 0.0;
            std::chrono::steady_clock::time_point submitted_at{};
        };

        VenueQualityModel &model_;
        std::array<TrackedOrder, MAX_TRACKED_ORDERS> tracked_orders_{};

        TrackedOrder *find(uint64_t client_order_id, Exchange exchange) noexcept {
            for (auto &tracked: tracked_orders_) {
                if (tracked.active && tracked.client_order_id == client_order_id &&
                    tracked.exchange == exchange) {
                    return &tracked;
                }
            }
            return nullptr;
        }

        TrackedOrder *acquire() noexcept {
            for (auto &tracked: tracked_orders_) {
                if (!tracked.active) {
                    return &tracked;
                }
            }
            return nullptr;
        }

        void release(uint64_t client_order_id, Exchange exchange) noexcept {
            TrackedOrder *tracked = find(client_order_id, exchange);
            if (tracked == nullptr) {
                return;
            }
            *tracked = TrackedOrder{};
        }

        static double realized_fill_quality_bps(const TrackedOrder &tracked, double fill_price,
                                                double mid_price) noexcept {
            if (mid_price <= 0.0 || fill_price <= 0.0) {
                return 0.0;
            }
            if (tracked.side == Side::BID) {
                return ((mid_price - fill_price) / mid_price) * 1e4;
            }
            return ((fill_price - mid_price) / mid_price) * 1e4;
        }
    };
}
