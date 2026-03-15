#include "coinbase_connector.hpp"

#include <string>

namespace trading {
namespace {

std::string order_payload(const Order& o) {
    return std::string("{\"product_id\":\"") + o.symbol +
           "\",\"side\":\"" + (o.side == Side::BID ? "BUY" : "SELL") +
           "\",\"order_type\":\"" + (o.type == OrderType::MARKET ? "MARKET" : "LIMIT") +
           "\",\"size\":\"" + std::to_string(o.quantity) + "\"}";
}

}  // namespace

ConnectorResult CoinbaseConnector::submit_to_venue(const Order& order,
                                                   const std::string& idempotency_key,
                                                   std::string& venue_order_id) {
    if (api_url().rfind("mock://", 0) == 0) {
        venue_order_id = std::string("CB-") + std::to_string(order.client_order_id);
        return ConnectorResult::OK;
    }

    const std::string payload = order_payload(order);
    const auto headers = auth_headers(payload, idempotency_key);
    const auto resp = http::post(api_url() + "/api/v3/brokerage/orders", payload, headers);
    if (resp.ok()) {
        venue_order_id = std::string("CB-") + std::to_string(order.client_order_id);
        return ConnectorResult::OK;
    }
    if (resp.status == 429) return ConnectorResult::ERROR_RATE_LIMIT;
    if (resp.status >= 400 && resp.status < 500) return ConnectorResult::ERROR_INVALID_ORDER;
    return ConnectorResult::ERROR_UNKNOWN;
}

ConnectorResult CoinbaseConnector::cancel_at_venue(const VenueOrderEntry& entry) {
    if (api_url().rfind("mock://", 0) == 0) return ConnectorResult::OK;
    const auto resp = http::post(api_url() + "/api/v3/brokerage/orders/batch_cancel",
                                 std::string("{\"order_ids\":[\"") + entry.venue_order_id + "\"]}");
    return resp.ok() ? ConnectorResult::OK : ConnectorResult::ERROR_UNKNOWN;
}

ConnectorResult CoinbaseConnector::cancel_all_at_venue(const char* symbol) {
    if (api_url().rfind("mock://", 0) == 0) return ConnectorResult::OK;
    const auto resp = http::post(api_url() + "/api/v3/brokerage/orders/batch_cancel",
                                 std::string("{\"product_id\":\"") + (symbol ? symbol : "") + "\"}");
    return resp.ok() ? ConnectorResult::OK : ConnectorResult::ERROR_UNKNOWN;
}

}  // namespace trading
