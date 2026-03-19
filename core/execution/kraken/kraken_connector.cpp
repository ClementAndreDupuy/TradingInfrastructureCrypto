#include "kraken_connector.hpp"

#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
#else
#include "../../common/json.hpp"
#endif

#include "../../common/symbol_mapper.hpp"

#include <sstream>
#include <string>

namespace trading {
namespace {

auto order_type_string(OrderType type) -> const char* {
    switch (type) {
    case OrderType::MARKET:
        return "market";
    case OrderType::LIMIT:
        return "limit";
    case OrderType::STOP_LIMIT:
        return "stop-loss-limit";
    default:
        return "limit";
    }
}

auto tif_string(TimeInForce tif) -> const char* {
    switch (tif) {
    case TimeInForce::IOC:
        return "ioc";
    case TimeInForce::GTC:
        return "gtc";
    case TimeInForce::FOK:
        return "fok";
    case TimeInForce::GTX:
        return "post_only";
    default:
        return "gtc";
    }
}

auto decimal_string(double value) -> std::string {
    std::ostringstream oss;
    oss.precision(15);
    oss << std::defaultfloat << value;
    return oss.str();
}

auto append_field(std::string& payload, const char* key, const std::string& value) -> void {
    if (!payload.empty()) {
        payload.push_back('&');
    }
    payload.append(key);
    payload.push_back('=');
    payload.append(value);
}

auto private_payload(std::string payload) -> std::string {
    const std::string nonce = std::to_string(http::now_ms());
    if (payload.empty()) {
        return std::string("nonce=") + nonce;
    }
    return std::string("nonce=") + nonce + "&" + payload;
}

auto build_order_payload(const Order& order) -> std::string {
    std::string payload;
    append_field(payload, "ordertype", order_type_string(order.type));
    append_field(payload, "pair", SymbolMapper::map_for_exchange(Exchange::KRAKEN, order.symbol));
    append_field(payload, "type", order.side == Side::BID ? "buy" : "sell");
    append_field(payload, "volume", decimal_string(order.quantity));
    if (order.type != OrderType::MARKET) {
        append_field(payload, "price", decimal_string(order.price));
    }
    if (order.type == OrderType::STOP_LIMIT) {
        append_field(payload, "price2", decimal_string(order.stop_price));
    }
    append_field(payload, "timeinforce", tif_string(order.tif));
    if (order.tif == TimeInForce::GTX) {
        append_field(payload, "oflags", "post");
    }
    return payload;
}

auto parse_order_id(const std::string& body, std::string& venue_order_id) -> bool {
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded()) {
        return false;
    }
    const auto& txids = json["result"]["txid"];
    if (!txids.is_array() || txids.empty() || !txids[0].is_string()) {
        return false;
    }
    venue_order_id = txids[0].get<std::string>();
    return !venue_order_id.empty();
}

auto parse_order_status(const std::string& raw) -> OrderState {
    if (raw == "open") {
        return OrderState::OPEN;
    }
    if (raw == "pending") {
        return OrderState::PENDING;
    }
    if (raw == "closed") {
        return OrderState::FILLED;
    }
    if (raw == "canceled" || raw == "expired") {
        return OrderState::CANCELED;
    }
    return OrderState::REJECTED;
}

auto parse_query_status(const std::string& body, FillUpdate& status) -> bool {
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded()) {
        return false;
    }
    const auto& result = json["result"];
    if (!result.is_object() || result.empty()) {
        return false;
    }
    const auto result_entry = result.begin();
    const auto& order = result_entry.value();

    status.cumulative_filled_qty = order.value("vol_exec", 0.0);
    status.fill_qty = status.cumulative_filled_qty;
    status.avg_fill_price = order.value("price", 0.0);
    status.fill_price = status.avg_fill_price;
    status.new_state = parse_order_status(order.value("status", std::string("")));
    status.local_ts_ns = http::now_ns();
    return true;
}

auto parse_cancel_result(const std::string& body) -> bool {
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded()) {
        return false;
    }
    return json["result"].value("count", int64_t{0}) > 0;
}

}

auto KrakenConnector::submit_to_venue(const Order& order, const std::string& idempotency_key,
                                      std::string& venue_order_id) -> ConnectorResult {
    std::string payload = private_payload(build_order_payload(order));
    append_field(payload, "cl_ord_id", idempotency_key);
    const auto resp = http::post(api_url() + "/0/private/AddOrder", payload,
                                 kraken_private_headers("/0/private/AddOrder", payload));
    if (!resp.ok()) {
        return classify_http_error(resp.status);
    }

    if (!parse_order_id(resp.body, venue_order_id)) {
        return ConnectorResult::ERROR_UNKNOWN;
    }
    return ConnectorResult::OK;
}

auto KrakenConnector::cancel_at_venue(const VenueOrderEntry& entry) -> ConnectorResult {
    const std::string payload = private_payload(std::string("txid=") + entry.venue_order_id);
    const auto resp = http::post(api_url() + "/0/private/CancelOrder", payload,
                                 kraken_private_headers("/0/private/CancelOrder", payload));
    if (!resp.ok()) {
        return classify_http_error(resp.status);
    }
    return parse_cancel_result(resp.body) ? ConnectorResult::OK
                                              : ConnectorResult::ERROR_UNKNOWN;
}

auto KrakenConnector::replace_at_venue(const VenueOrderEntry& entry, const Order& replacement,
                                       std::string& new_venue_order_id) -> ConnectorResult {
    std::string payload = private_payload(std::string("txid=") + entry.venue_order_id);
    append_field(payload, "order_qty", decimal_string(replacement.quantity));
    if (replacement.type != OrderType::MARKET) {
        append_field(payload, "limit_price", decimal_string(replacement.price));
    }
    if (replacement.type == OrderType::STOP_LIMIT) {
        append_field(payload, "trigger_price", decimal_string(replacement.stop_price));
    }
    const auto resp = http::post(api_url() + "/0/private/AmendOrder", payload,
                                 kraken_private_headers("/0/private/AmendOrder", payload));
    if (!resp.ok()) {
        return classify_http_error(resp.status);
    }
    new_venue_order_id = entry.venue_order_id;
    return ConnectorResult::OK;
}

auto KrakenConnector::query_at_venue(const VenueOrderEntry& entry,
                                     FillUpdate& status) -> ConnectorResult {
    const std::string payload = private_payload(std::string("txid=") + entry.venue_order_id);
    const auto resp = http::post(api_url() + "/0/private/QueryOrders", payload,
                                 kraken_private_headers("/0/private/QueryOrders", payload));
    if (!resp.ok()) {
        return classify_http_error(resp.status);
    }
    return parse_query_status(resp.body, status) ? ConnectorResult::OK
                                                 : ConnectorResult::ERROR_UNKNOWN;
}

auto KrakenConnector::cancel_all_at_venue(const char* symbol) -> ConnectorResult {
    (void)symbol;
    const std::string payload = private_payload(std::string());
    const auto resp = http::post(api_url() + "/0/private/CancelAll", payload,
                                 kraken_private_headers("/0/private/CancelAll", payload));
    if (!resp.ok()) {
        return classify_http_error(resp.status);
    }
    const auto json = nlohmann::json::parse(resp.body, nullptr, false);
    return (!json.is_discarded() && json.find("result") != json.end())
               ? ConnectorResult::OK
               : ConnectorResult::ERROR_UNKNOWN;
}

}

namespace trading {

auto KrakenConnector::fetch_reconciliation_snapshot(ReconciliationSnapshot& snapshot)
    -> ConnectorResult {
    snapshot.clear();

    const std::string open_payload = private_payload(std::string());
    const auto open_resp = http::post(api_url() + "/0/private/OpenOrders", open_payload,
                                      kraken_private_headers("/0/private/OpenOrders", open_payload));
    if (!open_resp.ok()) {
        return classify_http_error(open_resp.status);
    }

    const auto open_json = nlohmann::json::parse(open_resp.body, nullptr, false);
    const auto& open_map = open_json["result"]["open"];
    if (!open_map.is_object()) {
        return ConnectorResult::ERROR_UNKNOWN;
    }

    for (auto it = open_map.begin(); it != open_map.end(); ++it) {
        const auto& order_obj = it.value();
        const auto& descr = order_obj["descr"];
        ReconciledOrder order;
        copy_cstr(order.venue_order_id, sizeof(order.venue_order_id), it.key());
        copy_cstr(order.symbol, sizeof(order.symbol), descr.value("pair", std::string("")));
        order.side = descr.value("type", std::string("buy")) == "sell" ? Side::ASK : Side::BID;
        order.quantity = order_obj.value("vol", 0.0);
        order.filled_quantity = order_obj.value("vol_exec", 0.0);
        order.price = descr.value("price", 0.0);
        order.state = parse_order_status(order_obj.value("status", std::string("")));
        if (!snapshot.open_orders.push(order)) {
            return ConnectorResult::ERROR_UNKNOWN;
        }
    }

    const std::string balance_payload = private_payload(std::string());
    const auto balance_resp = http::post(api_url() + "/0/private/Balance", balance_payload,
                                         kraken_private_headers("/0/private/Balance",
                                                                balance_payload));
    if (!balance_resp.ok()) {
        return classify_http_error(balance_resp.status);
    }

    const auto balance_json = nlohmann::json::parse(balance_resp.body, nullptr, false);
    const auto& result = balance_json["result"];
    if (!result.is_object()) {
        return ConnectorResult::ERROR_UNKNOWN;
    }

    for (auto it = result.begin(); it != result.end(); ++it) {
        ReconciledBalance balance;
        copy_cstr(balance.asset, sizeof(balance.asset), it.key());
        balance.total = it.value().get<double>();
        balance.available = balance.total;
        if (!snapshot.balances.push(balance)) {
            return ConnectorResult::ERROR_UNKNOWN;
        }
    }

    const std::string trades_payload = private_payload(std::string());
    const auto trades_resp = http::post(api_url() + "/0/private/TradesHistory", trades_payload,
                                        kraken_private_headers("/0/private/TradesHistory",
                                                               trades_payload));
    if (trades_resp.status == 404 || trades_resp.status == 405 || trades_resp.status == 501) {
        return ConnectorResult::OK;
    }
    if (!trades_resp.ok()) {
        return classify_http_error(trades_resp.status);
    }

    const auto trades_json = nlohmann::json::parse(trades_resp.body, nullptr, false);
    const auto& trades = trades_json["result"]["trades"];
    if (!trades.is_object()) {
        return ConnectorResult::ERROR_UNKNOWN;
    }

    for (auto it = trades.begin(); it != trades.end(); ++it) {
        const auto& item = it.value();
        ReconciledFill fill;
        fill.exchange = Exchange::KRAKEN;
        copy_cstr(fill.venue_trade_id, sizeof(fill.venue_trade_id), it.key());
        copy_cstr(fill.venue_order_id, sizeof(fill.venue_order_id),
                  item.value("ordertxid", std::string("")));
        copy_cstr(fill.symbol, sizeof(fill.symbol), item.value("pair", std::string("")));
        fill.side = item.value("type", std::string("buy")) == "sell" ? Side::ASK : Side::BID;
        fill.quantity = item.value("vol", 0.0);
        fill.price = item.value("price", 0.0);
        fill.notional = item.value("cost", fill.quantity * fill.price);
        fill.fee = item.value("fee", 0.0);
        copy_cstr(fill.fee_asset, sizeof(fill.fee_asset), item.value("fee_ccy", std::string("")));
        fill.exchange_ts_ns = static_cast<int64_t>(item.value("time", 0.0) * 1000000000.0);
        if (!snapshot.fills.push(fill)) {
            return ConnectorResult::ERROR_UNKNOWN;
        }
    }

    return ConnectorResult::OK;
}

}
