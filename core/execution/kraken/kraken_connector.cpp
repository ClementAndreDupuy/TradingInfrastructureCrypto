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
    const auto resp = http::post(api_url() + "/0/private/CancelOrder", payload);
    if (!resp.ok())
        return classify_error(resp.status);
    return parse_kraken_cancel_ack(resp.body) ? ConnectorResult::OK : ConnectorResult::ERROR_UNKNOWN;
}

ConnectorResult KrakenConnector::cancel_all_at_venue(const char* symbol) {
    const std::string payload = std::string("pair=") + (symbol ? symbol : "");
    const auto resp = http::post(api_url() + "/0/private/CancelAllOrdersAfter", payload);
    if (!resp.ok())
        return classify_error(resp.status);
    const auto j = nlohmann::json::parse(resp.body, nullptr, false);
    return (!j.is_discarded() && j.find("result") != j.end()) ? ConnectorResult::OK
                                                               : ConnectorResult::ERROR_UNKNOWN;
}

} // namespace trading
