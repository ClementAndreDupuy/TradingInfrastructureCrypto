#include "kraken_connector.hpp"

#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
#else
#include "../../common/json.hpp"
#endif

#include <string>

namespace trading {
namespace {

std::string order_payload(const Order& o) {
    return std::string("pair=") + o.symbol + "&type=" + (o.side == Side::BID ? "buy" : "sell") +
           "&ordertype=" + (o.type == OrderType::MARKET ? "market" : "limit") +
           "&volume=" + std::to_string(o.quantity);
}

ConnectorResult classify_error(int status) {
    if (status == 429)
        return ConnectorResult::ERROR_RATE_LIMIT;
    if (status >= 400 && status < 500)
        return ConnectorResult::ERROR_INVALID_ORDER;
    return ConnectorResult::ERROR_UNKNOWN;
}

bool parse_kraken_order_id(const std::string& body, std::string& venue_order_id) {
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded())
        return false;
    const auto& txids = j["result"]["txid"];
    if (!txids.is_array() || txids.empty() || !txids[0].is_string())
        return false;
    venue_order_id = txids[0].get<std::string>();
    return !venue_order_id.empty();
}

OrderState parse_kraken_status(const std::string& raw) {
    if (raw == "open")
        return OrderState::OPEN;
    if (raw == "pending")
        return OrderState::PENDING;
    if (raw == "closed")
        return OrderState::FILLED;
    if (raw == "canceled" || raw == "expired")
        return OrderState::CANCELED;
    return OrderState::REJECTED;
}

bool parse_kraken_query(const std::string& body, FillUpdate& status) {
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded())
        return false;
    const auto& result = j["result"];
    if (!result.is_object() || result.empty())
        return false;
    const auto it = result.begin();
    const auto& order = it.value();

    status.cumulative_filled_qty = order.value("vol_exec", 0.0);
    status.fill_qty = status.cumulative_filled_qty;
    status.avg_fill_price = order.value("price", 0.0);
    status.fill_price = status.avg_fill_price;
    status.new_state = parse_kraken_status(order.value("status", std::string("")));
    status.local_ts_ns = http::now_ns();
    return true;
}

bool parse_kraken_cancel_ack(const std::string& body) {
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded())
        return false;
    return j["result"].value("count", int64_t{0}) > 0;
}

} // namespace

ConnectorResult KrakenConnector::submit_to_venue(const Order& order,
                                                 const std::string& idempotency_key,
                                                 std::string& venue_order_id) {
    const std::string payload = order_payload(order);
    const auto headers = auth_headers(payload, idempotency_key);
    const auto resp = http::post(api_url() + "/0/private/AddOrder", payload, headers);
    if (!resp.ok())
        return classify_error(resp.status);

    if (!parse_kraken_order_id(resp.body, venue_order_id))
        return ConnectorResult::ERROR_UNKNOWN;
    return ConnectorResult::OK;
}

ConnectorResult KrakenConnector::cancel_at_venue(const VenueOrderEntry& entry) {
    const std::string payload = std::string("txid=") + entry.venue_order_id;
    const auto resp =
        http::post(api_url() + "/0/private/CancelOrder", payload, auth_headers(payload));
    if (!resp.ok())
        return classify_error(resp.status);
    return parse_kraken_cancel_ack(resp.body) ? ConnectorResult::OK
                                              : ConnectorResult::ERROR_UNKNOWN;
}

ConnectorResult KrakenConnector::replace_at_venue(const VenueOrderEntry& entry,
                                                  const Order& replacement,
                                                  std::string& new_venue_order_id) {
    const std::string payload =
        std::string("txid=") + entry.venue_order_id + "&" + order_payload(replacement);
    const auto resp =
        http::post(api_url() + "/0/private/EditOrder", payload, auth_headers(payload));
    if (!resp.ok())
        return classify_error(resp.status);
    return parse_kraken_order_id(resp.body, new_venue_order_id) ? ConnectorResult::OK
                                                                : ConnectorResult::ERROR_UNKNOWN;
}

ConnectorResult KrakenConnector::query_at_venue(const VenueOrderEntry& entry, FillUpdate& status) {
    const std::string payload = std::string("txid=") + entry.venue_order_id;
    const auto resp =
        http::post(api_url() + "/0/private/QueryOrders", payload, auth_headers(payload));
    if (!resp.ok())
        return classify_error(resp.status);
    return parse_kraken_query(resp.body, status) ? ConnectorResult::OK
                                                 : ConnectorResult::ERROR_UNKNOWN;
}

ConnectorResult KrakenConnector::cancel_all_at_venue(const char* symbol) {
    const std::string payload = std::string("pair=") + (symbol ? symbol : "");
    const auto resp =
        http::post(api_url() + "/0/private/CancelAllOrdersAfter", payload, auth_headers(payload));
    if (!resp.ok())
        return classify_error(resp.status);
    const auto j = nlohmann::json::parse(resp.body, nullptr, false);
    return (!j.is_discarded() && j.find("result") != j.end()) ? ConnectorResult::OK
                                                              : ConnectorResult::ERROR_UNKNOWN;
}

} // namespace trading
