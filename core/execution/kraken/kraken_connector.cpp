#include "kraken_connector.hpp"

#include <string>

namespace trading {
namespace {

std::string order_payload(const Order& o) {
    return std::string("pair=") + o.symbol +
           "&type=" + (o.side == Side::BID ? "buy" : "sell") +
           "&ordertype=" + (o.type == OrderType::MARKET ? "market" : "limit") +
           "&volume=" + std::to_string(o.quantity);
}

}  // namespace

ConnectorResult KrakenConnector::submit_to_venue(const Order& order,
                                                 const std::string& idempotency_key,
                                                 std::string& venue_order_id) {
    if (api_url().rfind("mock://", 0) == 0) {
        venue_order_id = std::string("KR-") + std::to_string(order.client_order_id);
        return ConnectorResult::OK;
    }

    const std::string payload = order_payload(order);
    const auto headers = auth_headers(payload, idempotency_key);
    const auto resp = http::post(api_url() + "/0/private/AddOrder", payload, headers);
    if (resp.ok()) {
        venue_order_id = std::string("KR-") + std::to_string(order.client_order_id);
        return ConnectorResult::OK;
    }
    if (resp.status == 429) return ConnectorResult::ERROR_RATE_LIMIT;
    if (resp.status >= 400 && resp.status < 500) return ConnectorResult::ERROR_INVALID_ORDER;
    return ConnectorResult::ERROR_UNKNOWN;
}

ConnectorResult KrakenConnector::cancel_at_venue(const VenueOrderEntry& entry) {
    if (api_url().rfind("mock://", 0) == 0) return ConnectorResult::OK;
    const std::string payload = std::string("txid=") + entry.venue_order_id;
    const auto resp = http::post(api_url() + "/0/private/CancelOrder", payload);
    return resp.ok() ? ConnectorResult::OK : ConnectorResult::ERROR_UNKNOWN;
}

ConnectorResult KrakenConnector::cancel_all_at_venue(const char* symbol) {
    if (api_url().rfind("mock://", 0) == 0) return ConnectorResult::OK;
    const std::string payload = std::string("pair=") + (symbol ? symbol : "");
    const auto resp = http::post(api_url() + "/0/private/CancelAllOrdersAfter", payload);
    return resp.ok() ? ConnectorResult::OK : ConnectorResult::ERROR_UNKNOWN;
}

}  // namespace trading
