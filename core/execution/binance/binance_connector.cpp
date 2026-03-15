#include "binance_connector.hpp"

#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
#else
#include "../../common/json.hpp"
#endif

#include <string>

namespace trading {
namespace {

std::string order_payload(const Order& o) {
    return std::string("{\"symbol\":\"") + o.symbol + "\",\"side\":\"" +
           (o.side == Side::BID ? "BUY" : "SELL") + "\",\"type\":\"" +
           (o.type == OrderType::MARKET ? "MARKET" : "LIMIT") +
           "\",\"quantity\":" + std::to_string(o.quantity) +
           ",\"price\":" + std::to_string(o.price) + "}";
}

ConnectorResult classify_error(int status) {
    if (status == 429)
        return ConnectorResult::ERROR_RATE_LIMIT;
    if (status >= 400 && status < 500)
        return ConnectorResult::ERROR_INVALID_ORDER;
    return ConnectorResult::ERROR_UNKNOWN;
}

bool parse_binance_order_id(const std::string& body, std::string& venue_order_id) {
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded())
        return false;

    const auto raw_id = j.value("orderId", std::string(""));
    if (!raw_id.empty()) {
        venue_order_id = raw_id;
        return true;
    }

    const uint64_t numeric_id = j.value("orderId", uint64_t{0});
    if (numeric_id != 0) {
        venue_order_id = std::to_string(numeric_id);
        return true;
    }
    return false;
}

bool parse_binance_cancel_ack(const std::string& body) {
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded())
        return false;
    if (j.find("orderId") != j.end())
        return true;
    return j.value("status", std::string("")) == "CANCELED";
}

} // namespace

ConnectorResult BinanceConnector::submit_to_venue(const Order& order,
                                                  const std::string& idempotency_key,
                                                  std::string& venue_order_id) {
    const std::string payload = order_payload(order);
    const auto headers = auth_headers(payload, idempotency_key);
    const auto resp = http::post(api_url() + "/api/v3/order", payload, headers);
    if (!resp.ok())
        return classify_error(resp.status);

    if (!parse_binance_order_id(resp.body, venue_order_id))
        return ConnectorResult::ERROR_UNKNOWN;
    return ConnectorResult::OK;
}

ConnectorResult BinanceConnector::cancel_at_venue(const VenueOrderEntry& entry) {
    const auto resp =
        http::del(api_url() + "/api/v3/order?orderId=" + std::string(entry.venue_order_id));
    if (!resp.ok())
        return classify_error(resp.status);
    return parse_binance_cancel_ack(resp.body) ? ConnectorResult::OK
                                               : ConnectorResult::ERROR_UNKNOWN;
}

ConnectorResult BinanceConnector::cancel_all_at_venue(const char* symbol) {
    const auto resp =
        http::del(api_url() + "/api/v3/openOrders?symbol=" + std::string(symbol ? symbol : ""));
    if (!resp.ok())
        return classify_error(resp.status);
    const auto j = nlohmann::json::parse(resp.body, nullptr, false);
    return (!j.is_discarded() && j.is_array()) ? ConnectorResult::OK
                                               : ConnectorResult::ERROR_UNKNOWN;
}

} // namespace trading
