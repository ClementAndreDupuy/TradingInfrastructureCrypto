#include "coinbase_connector.hpp"

#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
#else
#include "../../common/json.hpp"
#endif

#include <string>

namespace trading {
namespace {

std::string order_payload(const Order& o) {
    return std::string("{\"product_id\":\"") + o.symbol + "\",\"side\":\"" +
           (o.side == Side::BID ? "BUY" : "SELL") + "\",\"order_type\":\"" +
           (o.type == OrderType::MARKET ? "MARKET" : "LIMIT") + "\",\"size\":\"" +
           std::to_string(o.quantity) + "\"}";
}

ConnectorResult classify_error(int status) {
    if (status == 429)
        return ConnectorResult::ERROR_RATE_LIMIT;
    if (status >= 400 && status < 500)
        return ConnectorResult::ERROR_INVALID_ORDER;
    return ConnectorResult::ERROR_UNKNOWN;
}

bool parse_coinbase_order_id(const std::string& body, std::string& venue_order_id) {
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded())
        return false;
    venue_order_id = j["success_response"].value("order_id", std::string(""));
    return !venue_order_id.empty();
}

bool parse_coinbase_cancel_ack(const std::string& body) {
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded())
        return false;
    const auto& results = j["results"];
    if (!results.is_array() || results.empty())
        return false;
    return results[0].value("success", false);
}

} // namespace

ConnectorResult CoinbaseConnector::submit_to_venue(const Order& order,
                                                   const std::string& idempotency_key,
                                                   std::string& venue_order_id) {
    const std::string payload = order_payload(order);
    const auto headers = auth_headers(payload, idempotency_key);
    const auto resp = http::post(api_url() + "/api/v3/brokerage/orders", payload, headers);
    if (!resp.ok())
        return classify_error(resp.status);

    if (!parse_coinbase_order_id(resp.body, venue_order_id))
        return ConnectorResult::ERROR_UNKNOWN;
    return ConnectorResult::OK;
}

ConnectorResult CoinbaseConnector::cancel_at_venue(const VenueOrderEntry& entry) {
    const auto resp = http::post(api_url() + "/api/v3/brokerage/orders/batch_cancel",
                                 std::string("{\"order_ids\":[\"") + entry.venue_order_id + "\"]}");
    if (!resp.ok())
        return classify_error(resp.status);
    return parse_coinbase_cancel_ack(resp.body) ? ConnectorResult::OK
                                                : ConnectorResult::ERROR_UNKNOWN;
}

ConnectorResult CoinbaseConnector::cancel_all_at_venue(const char* symbol) {
    const auto resp =
        http::post(api_url() + "/api/v3/brokerage/orders/batch_cancel",
                   std::string("{\"product_id\":\"") + (symbol ? symbol : "") + "\"}");
    if (!resp.ok())
        return classify_error(resp.status);
    return parse_coinbase_cancel_ack(resp.body) ? ConnectorResult::OK
                                                : ConnectorResult::ERROR_UNKNOWN;
}

} // namespace trading
