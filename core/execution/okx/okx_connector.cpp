#include "okx_connector.hpp"

#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
#else
#include "../../common/json.hpp"
#endif

#include <string>

namespace trading {
namespace {

std::string order_payload(const Order& o) {
    return std::string("{\"instId\":\"") + o.symbol + "\",\"side\":\"" +
           (o.side == Side::BID ? "buy" : "sell") + "\",\"ordType\":\"" +
           (o.type == OrderType::MARKET ? "market" : "limit") + "\",\"sz\":\"" +
           std::to_string(o.quantity) + "\"}";
}

ConnectorResult classify_error(int status) {
    if (status == 429)
        return ConnectorResult::ERROR_RATE_LIMIT;
    if (status >= 400 && status < 500)
        return ConnectorResult::ERROR_INVALID_ORDER;
    return ConnectorResult::ERROR_UNKNOWN;
}

bool parse_okx_order_id(const std::string& body, std::string& venue_order_id) {
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded())
        return false;
    const auto& data = j["data"];
    if (!data.is_array() || data.empty())
        return false;
    venue_order_id = data[0].value("ordId", std::string(""));
    return !venue_order_id.empty();
}

bool parse_okx_cancel_ack(const std::string& body) {
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded())
        return false;
    const auto& data = j["data"];
    if (!data.is_array() || data.empty())
        return false;
    return data[0].value("sCode", std::string("")) == "0";
}

} // namespace

ConnectorResult OkxConnector::submit_to_venue(const Order& order,
                                              const std::string& idempotency_key,
                                              std::string& venue_order_id) {
    const std::string payload = order_payload(order);
    const auto headers = auth_headers(payload, idempotency_key);
    const auto resp = http::post(api_url() + "/api/v5/trade/order", payload, headers);
    if (!resp.ok())
        return classify_error(resp.status);

    if (!parse_okx_order_id(resp.body, venue_order_id))
        return ConnectorResult::ERROR_UNKNOWN;
    return ConnectorResult::OK;
}

ConnectorResult OkxConnector::cancel_at_venue(const VenueOrderEntry& entry) {
    const std::string payload = std::string("{\"ordId\":\"") + entry.venue_order_id + "\"}";
    const auto resp = http::post(api_url() + "/api/v5/trade/cancel-order", payload);
    if (!resp.ok())
        return classify_error(resp.status);
    return parse_okx_cancel_ack(resp.body) ? ConnectorResult::OK : ConnectorResult::ERROR_UNKNOWN;
}

ConnectorResult OkxConnector::cancel_all_at_venue(const char* symbol) {
    const std::string payload = std::string("{\"instId\":\"") + (symbol ? symbol : "") + "\"}";
    const auto resp = http::post(api_url() + "/api/v5/trade/cancel-batch-orders", payload);
    if (!resp.ok())
        return classify_error(resp.status);
    return parse_okx_cancel_ack(resp.body) ? ConnectorResult::OK : ConnectorResult::ERROR_UNKNOWN;
}

} // namespace trading
