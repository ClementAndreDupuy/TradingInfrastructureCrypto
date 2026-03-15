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



OrderState parse_okx_status(const std::string& raw) {
    if (raw == "live")
        return OrderState::OPEN;
    if (raw == "partially_filled")
        return OrderState::PARTIALLY_FILLED;
    if (raw == "filled")
        return OrderState::FILLED;
    if (raw == "canceled")
        return OrderState::CANCELED;
    return OrderState::REJECTED;
}

bool parse_okx_query(const std::string& body, FillUpdate& status) {
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded())
        return false;
    const auto& data = j["data"];
    if (!data.is_array() || data.empty())
        return false;
    const auto& first = data[0];
    status.cumulative_filled_qty = first.value("accFillSz", 0.0);
    status.fill_qty = status.cumulative_filled_qty;
    status.avg_fill_price = first.value("avgPx", 0.0);
    status.fill_price = status.avg_fill_price;
    status.new_state = parse_okx_status(first.value("state", std::string("")));
    status.local_ts_ns = http::now_ns();
    return true;
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
    const auto resp = http::post(api_url() + "/api/v5/trade/cancel-order", payload, auth_headers(payload));
    if (!resp.ok())
        return classify_error(resp.status);
    return parse_okx_cancel_ack(resp.body) ? ConnectorResult::OK : ConnectorResult::ERROR_UNKNOWN;
}


ConnectorResult OkxConnector::replace_at_venue(const VenueOrderEntry& entry, const Order& replacement,
                                               std::string& new_venue_order_id) {
    const std::string payload = std::string("{\"ordId\":\"") + entry.venue_order_id +
                                "\",\"newPx\":\"" + std::to_string(replacement.price) +
                                "\",\"newSz\":\"" + std::to_string(replacement.quantity) + "\"}";
    const auto resp = http::post(api_url() + "/api/v5/trade/amend-order", payload, auth_headers(payload));
    if (!resp.ok())
        return classify_error(resp.status);
    return parse_okx_order_id(resp.body, new_venue_order_id) ? ConnectorResult::OK
                                                              : ConnectorResult::ERROR_UNKNOWN;
}

ConnectorResult OkxConnector::query_at_venue(const VenueOrderEntry& entry, FillUpdate& status) {
    const auto resp = http::get(api_url() + "/api/v5/trade/order?ordId=" + std::string(entry.venue_order_id),
                               auth_headers(std::string("ordId=") + entry.venue_order_id));
    if (!resp.ok())
        return classify_error(resp.status);
    return parse_okx_query(resp.body, status) ? ConnectorResult::OK : ConnectorResult::ERROR_UNKNOWN;
}

ConnectorResult OkxConnector::cancel_all_at_venue(const char* symbol) {
    const std::string payload = std::string("{\"instId\":\"") + (symbol ? symbol : "") + "\"}";
    const auto resp = http::post(api_url() + "/api/v5/trade/cancel-batch-orders", payload,
                                auth_headers(payload));
    if (!resp.ok())
        return classify_error(resp.status);
    return parse_okx_cancel_ack(resp.body) ? ConnectorResult::OK : ConnectorResult::ERROR_UNKNOWN;
}

} // namespace trading
