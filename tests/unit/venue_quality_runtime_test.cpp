#include "core/engine/venue_quality_runtime.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace trading::engine {
namespace {

TEST(VenueQualityRuntimeTest, RejectedSubmitUpdatesRejectRateWithoutTrackingOrder) {
    VenueQualityModel model;
    VenueQualityRuntime runtime(model);
    Order order;
    order.client_order_id = 7;
    order.exchange = Exchange::OKX;
    order.quantity = 0.5;

    runtime.on_submit(order, ConnectorResult::ERROR_INVALID_ORDER, std::chrono::steady_clock::now());

    EXPECT_GT(model.snapshot(Exchange::OKX).reject_rate, 0.0);
}

TEST(VenueQualityRuntimeTest, FillAndCancelUseRealizedOutcomes) {
    VenueQualityModel model;
    VenueQualityRuntime runtime(model);
    Order order;
    order.client_order_id = 11;
    order.exchange = Exchange::KRAKEN;
    order.side = Side::BID;
    order.tif = TimeInForce::GTX;
    order.quantity = 1.0;

    const auto submit_time = std::chrono::steady_clock::now();
    runtime.on_submit(order, ConnectorResult::OK, submit_time);

    FillUpdate partial_fill;
    partial_fill.client_order_id = order.client_order_id;
    partial_fill.fill_price = 100.0;
    partial_fill.fill_qty = 0.4;
    partial_fill.cumulative_filled_qty = 0.4;
    partial_fill.new_state = OrderState::PARTIALLY_FILLED;
    runtime.on_fill(order.exchange, partial_fill, submit_time + std::chrono::milliseconds(5),
                    [](Exchange) { return 100.2; });

    FillUpdate canceled;
    canceled.client_order_id = order.client_order_id;
    canceled.cumulative_filled_qty = 0.4;
    canceled.new_state = OrderState::CANCELED;
    runtime.on_fill(order.exchange, canceled, submit_time + std::chrono::milliseconds(20),
                    [](Exchange) { return 100.2; });

    const VenueQualitySnapshot snap = model.snapshot(Exchange::KRAKEN);
    EXPECT_GT(snap.fill_probability, 0.4);
    EXPECT_GT(snap.passive_markout_bps, 0.0);
    EXPECT_GT(snap.cancel_latency_penalty_bps, 0.0);
}

}
}
