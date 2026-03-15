#include "okx_connector.hpp"

#include <string>

namespace trading {
namespace {

std::string order_payload(const Order& o) {
    return std::string("{\"instId\":\"") + o.symbol + "\",\"side\":\"" +
           (o.side == Side::BID ? "buy" : "sell") + "\",\"ordType\":\"" +
           (o.type == OrderType::MARKET ? "market" : "limit") + "\",\"sz\":\"" +
           std::to_string(o.quantity) + "\"}";
}

} // namespace

ConnectorResult OkxConnector::submit_to_venue(const Order& order,
                                              const std::string& idempotency_key,
                                              std::string& venue_order_id) {
    if (api_url().rfind("mock://", 0) == 0) {
        venue_order_id = std::string("OK-") + std::to_string(order.client_order_id);
        return ConnectorResult::OK;
    }

    const std::string payload = order_payload(order);
    const auto headers = auth_headers(payload, idempotency_key);
    const auto resp = http::post(api_url() + "/api/v5/trade/order", payload, headers);
    if (resp.ok()) {
        venue_order_id = std::string("OK-") + std::to_string(order.client_order_id);
        return ConnectorResult::OK;
    }
    if (resp.status == 429)
        return ConnectorResult::ERROR_RATE_LIMIT;
    if (resp.status >= 400 && resp.status < 500)
        return ConnectorResult::ERROR_INVALID_ORDER;
    return ConnectorResult::ERROR_UNKNOWN;
}

ConnectorResult OkxConnector::cancel_at_venue(const VenueOrderEntry& entry) {
    if (api_url().rfind("mock://", 0) == 0)
        return ConnectorResult::OK;
    const std::string payload = std::string("{\"ordId\":\"") + entry.venue_order_id + "\"}";
    const auto resp = http::post(api_url() + "/api/v5/trade/cancel-order", payload);
    return resp.ok() ? ConnectorResult::OK : ConnectorResult::ERROR_UNKNOWN;
}

ConnectorResult OkxConnector::cancel_all_at_venue(const char* symbol) {
    if (api_url().rfind("mock://", 0) == 0)
        return ConnectorResult::OK;
    const std::string payload = std::string("{\"instId\":\"") + (symbol ? symbol : "") + "\"}";
    const auto resp = http::post(api_url() + "/api/v5/trade/cancel-batch-orders", payload);
    return resp.ok() ? ConnectorResult::OK : ConnectorResult::ERROR_UNKNOWN;
}

} // namespace trading
