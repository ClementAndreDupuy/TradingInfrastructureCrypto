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

OrderState parse_coinbase_status(const std::string& raw) {
    if (raw == "OPEN")
        return OrderState::OPEN;
    if (raw == "FILLED")
        return OrderState::FILLED;
    if (raw == "CANCELLED")
        return OrderState::CANCELED;
    if (raw == "REJECTED" || raw == "FAILED")
        return OrderState::REJECTED;
    return OrderState::PENDING;
}

bool parse_coinbase_query(const std::string& body, FillUpdate& status) {
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded())
        return false;
    const auto& order = j["order"];
    if (!order.is_object())
        return false;

    status.cumulative_filled_qty = order.value("filled_size", 0.0);
    status.fill_qty = status.cumulative_filled_qty;
    status.avg_fill_price = order.value("average_filled_price", 0.0);
    status.fill_price = status.avg_fill_price;
    status.new_state = parse_coinbase_status(order.value("status", std::string("")));
    status.local_ts_ns = http::now_ns();
    return true;
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
    const std::string payload = std::string("{\"order_ids\":[\"") + entry.venue_order_id + "\"]}";
    const auto resp = http::post(api_url() + "/api/v3/brokerage/orders/batch_cancel", payload,
                                 auth_headers(payload));
    if (!resp.ok())
        return classify_error(resp.status);
    return parse_coinbase_cancel_ack(resp.body) ? ConnectorResult::OK
                                                : ConnectorResult::ERROR_UNKNOWN;
}

ConnectorResult CoinbaseConnector::replace_at_venue(const VenueOrderEntry& entry,
                                                    const Order& replacement,
                                                    std::string& new_venue_order_id) {
    const std::string payload = std::string("{\"order_id\":\"") + entry.venue_order_id +
                                "\",\"size\":\"" + std::to_string(replacement.quantity) +
                                "\",\"limit_price\":\"" + std::to_string(replacement.price) + "\"}";
    const auto resp =
        http::post(api_url() + "/api/v3/brokerage/orders/edit", payload, auth_headers(payload));
    if (!resp.ok())
        return classify_error(resp.status);
    return parse_coinbase_order_id(resp.body, new_venue_order_id) ? ConnectorResult::OK
                                                                  : ConnectorResult::ERROR_UNKNOWN;
}

ConnectorResult CoinbaseConnector::query_at_venue(const VenueOrderEntry& entry,
                                                  FillUpdate& status) {
    const auto resp = http::get(api_url() + "/api/v3/brokerage/orders/historical/" +
                                    std::string(entry.venue_order_id),
                                auth_headers(entry.venue_order_id));
    if (!resp.ok())
        return classify_error(resp.status);
    return parse_coinbase_query(resp.body, status) ? ConnectorResult::OK
                                                   : ConnectorResult::ERROR_UNKNOWN;
}

ConnectorResult CoinbaseConnector::cancel_all_at_venue(const char* symbol) {
    const std::string payload = std::string("{\"product_id\":\"") + (symbol ? symbol : "") + "\"}";
    const auto resp = http::post(api_url() + "/api/v3/brokerage/orders/batch_cancel", payload,
                                 auth_headers(payload));
    if (!resp.ok())
        return classify_error(resp.status);
    return parse_coinbase_cancel_ack(resp.body) ? ConnectorResult::OK
                                                : ConnectorResult::ERROR_UNKNOWN;
}

} // namespace trading
