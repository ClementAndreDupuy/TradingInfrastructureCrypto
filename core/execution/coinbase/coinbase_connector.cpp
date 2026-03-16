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
    if (status == 401 || status == 403)
        return ConnectorResult::AUTH_FAILED;
    if (status == 429)
        return ConnectorResult::ERROR_RATE_LIMIT;
    if (status >= 500)
        return ConnectorResult::ERROR_REST_FAILURE;
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
    const auto headers = auth_headers("POST", "/api/v3/brokerage/orders", payload, idempotency_key);
    const auto resp = http::post(api_url() + "/api/v3/brokerage/orders", payload, headers);
    if (!resp.ok())
        return classify_error(resp.status);

    if (!parse_coinbase_order_id(resp.body, venue_order_id))
        return ConnectorResult::ERROR_UNKNOWN;
    return ConnectorResult::OK;
}

ConnectorResult CoinbaseConnector::cancel_at_venue(const VenueOrderEntry& entry) {
    const std::string payload = std::string("{\"order_ids\":[\"") + entry.venue_order_id + "\"]}";
    const auto resp =
        http::post(api_url() + "/api/v3/brokerage/orders/batch_cancel", payload,
                   auth_headers("POST", "/api/v3/brokerage/orders/batch_cancel", payload));
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
    const auto resp = http::post(api_url() + "/api/v3/brokerage/orders/edit", payload,
                                 auth_headers("POST", "/api/v3/brokerage/orders/edit", payload));
    if (!resp.ok())
        return classify_error(resp.status);
    return parse_coinbase_order_id(resp.body, new_venue_order_id) ? ConnectorResult::OK
                                                                  : ConnectorResult::ERROR_UNKNOWN;
}

ConnectorResult CoinbaseConnector::query_at_venue(const VenueOrderEntry& entry,
                                                  FillUpdate& status) {
    const auto resp = http::get(
        api_url() + "/api/v3/brokerage/orders/historical/" + std::string(entry.venue_order_id),
        auth_headers(
            "GET", std::string("/api/v3/brokerage/orders/historical/") + entry.venue_order_id, ""));
    if (!resp.ok())
        return classify_error(resp.status);
    return parse_coinbase_query(resp.body, status) ? ConnectorResult::OK
                                                   : ConnectorResult::ERROR_UNKNOWN;
}

ConnectorResult CoinbaseConnector::cancel_all_at_venue(const char* symbol) {
    const std::string payload = std::string("{\"product_id\":\"") + (symbol ? symbol : "") + "\"}";
    const auto resp =
        http::post(api_url() + "/api/v3/brokerage/orders/batch_cancel", payload,
                   auth_headers("POST", "/api/v3/brokerage/orders/batch_cancel", payload));
    if (!resp.ok())
        return classify_error(resp.status);
    return parse_coinbase_cancel_ack(resp.body) ? ConnectorResult::OK
                                                : ConnectorResult::ERROR_UNKNOWN;
}

} // namespace trading

namespace trading {

ConnectorResult CoinbaseConnector::fetch_reconciliation_snapshot(ReconciliationSnapshot& snapshot) {
    snapshot.clear();

    const auto open_orders =
        http::get(api_url() + "/api/v3/brokerage/orders/historical/batch",
                  auth_headers("GET", "/api/v3/brokerage/orders/historical/batch", ""));
    if (!open_orders.ok())
        return classify_error(open_orders.status);

    const auto open_json = nlohmann::json::parse(open_orders.body, nullptr, false);
    const auto& orders = open_json["orders"];
    if (!orders.is_array())
        return ConnectorResult::ERROR_UNKNOWN;

    for (const auto& item : orders) {
        ReconciledOrder order;
        copy_cstr(order.venue_order_id, sizeof(order.venue_order_id),
                  item.value("order_id", std::string("")));
        copy_cstr(order.symbol, sizeof(order.symbol), item.value("product_id", std::string("")));
        order.side = item.value("side", std::string("BUY")) == "SELL" ? Side::ASK : Side::BID;
        order.quantity = item.value("base_size", 0.0);
        order.filled_quantity = item.value("filled_size", 0.0);
        order.price = item.value("limit_price", 0.0);
        order.state = parse_coinbase_status(item.value("status", std::string("")));
        if (!snapshot.open_orders.push(order))
            return ConnectorResult::ERROR_UNKNOWN;
    }

    const auto account_resp = http::get(api_url() + "/api/v3/brokerage/accounts",
                                        auth_headers("GET", "/api/v3/brokerage/accounts", ""));
    if (!account_resp.ok())
        return classify_error(account_resp.status);

    const auto account_json = nlohmann::json::parse(account_resp.body, nullptr, false);
    const auto& accounts = account_json["accounts"];
    if (!accounts.is_array())
        return ConnectorResult::ERROR_UNKNOWN;

    for (const auto& item : accounts) {
        ReconciledBalance balance;
        copy_cstr(balance.asset, sizeof(balance.asset), item.value("currency", std::string("")));
        balance.total =
            item["available_balance"].value("value", 0.0) + item["hold"].value("value", 0.0);
        balance.available = item["available_balance"].value("value", 0.0);
        if (!snapshot.balances.push(balance))
            return ConnectorResult::ERROR_UNKNOWN;
    }

    const auto pos_resp = http::get(api_url() + "/api/v3/brokerage/positions",
                                    auth_headers("GET", "/api/v3/brokerage/positions", ""));
    if (!pos_resp.ok())
        return classify_error(pos_resp.status);

    const auto pos_json = nlohmann::json::parse(pos_resp.body, nullptr, false);
    const auto& positions = pos_json["positions"];
    if (!positions.is_array())
        return ConnectorResult::ERROR_UNKNOWN;

    for (const auto& item : positions) {
        ReconciledPosition position;
        copy_cstr(position.symbol, sizeof(position.symbol),
                  item.value("product_id", std::string("")));
        position.quantity = item.value("size", 0.0);
        position.avg_entry_price = item.value("average_entry_price", 0.0);
        if (!snapshot.positions.push(position))
            return ConnectorResult::ERROR_UNKNOWN;
    }

    const auto fills_resp =
        http::get(api_url() + "/api/v3/brokerage/orders/historical/fills",
                  auth_headers("GET", "/api/v3/brokerage/orders/historical/fills", ""));
    if (fills_resp.status == 404 || fills_resp.status == 405 || fills_resp.status == 501)
        return ConnectorResult::OK;
    if (!fills_resp.ok())
        return classify_error(fills_resp.status);

    const auto fills_json = nlohmann::json::parse(fills_resp.body, nullptr, false);
    const auto& fills = fills_json["fills"];
    if (!fills.is_array())
        return ConnectorResult::ERROR_UNKNOWN;

    for (const auto& item : fills) {
        ReconciledFill fill;
        fill.exchange = Exchange::COINBASE;
        copy_cstr(fill.venue_trade_id, sizeof(fill.venue_trade_id),
                  item.value("trade_id", std::string("")));
        copy_cstr(fill.venue_order_id, sizeof(fill.venue_order_id),
                  item.value("order_id", std::string("")));
        copy_cstr(fill.symbol, sizeof(fill.symbol), item.value("product_id", std::string("")));
        fill.side = item.value("side", std::string("BUY")) == "SELL" ? Side::ASK : Side::BID;
        fill.quantity = item.value("size", 0.0);
        fill.price = item.value("price", 0.0);
        fill.notional = fill.quantity * fill.price;
        fill.fee = item.value("commission", 0.0);
        copy_cstr(fill.fee_asset, sizeof(fill.fee_asset),
                  item.value("commission_currency", std::string("")));
        fill.exchange_ts_ns = item.value("trade_time_ns", static_cast<int64_t>(0));
        if (!snapshot.fills.push(fill))
            return ConnectorResult::ERROR_UNKNOWN;
    }

    return ConnectorResult::OK;
}

} // namespace trading
