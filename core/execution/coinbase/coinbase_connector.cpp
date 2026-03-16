#include "coinbase_connector.hpp"

#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
#else
#include "../../common/json.hpp"
#endif

#include <string>

namespace trading {
namespace {

auto order_payload(const Order& order) -> std::string {
    return std::string(R"({"product_id":")") + order.symbol + R"(","side":")" +
           (order.side == Side::BID ? "BUY" : "SELL") + R"(","order_type":")" +
           (order.type == OrderType::MARKET ? "MARKET" : "LIMIT") + R"(","size":")" +
           std::to_string(order.quantity) + R"("})";
}

auto classify_error(int status) -> ConnectorResult {
    if (status == 401 || status == 403) {
        return ConnectorResult::AUTH_FAILED;
    }
    if (status == 429) {
        return ConnectorResult::ERROR_RATE_LIMIT;
    }
    if (status >= 500) {
        return ConnectorResult::ERROR_REST_FAILURE;
    }
    if (status >= 400 && status < 500) {
        return ConnectorResult::ERROR_INVALID_ORDER;
    }
    return ConnectorResult::ERROR_UNKNOWN;
}

auto parse_coinbase_order_id(const std::string& body, std::string& venue_order_id) -> bool {
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded()) {
        return false;
    }
    venue_order_id = json["success_response"].value("order_id", std::string(""));
    return !venue_order_id.empty();
}

auto parse_coinbase_status(const std::string& raw) -> OrderState {
    if (raw == "OPEN") {
        return OrderState::OPEN;
    }
    if (raw == "FILLED") {
        return OrderState::FILLED;
    }
    if (raw == "CANCELLED") {
        return OrderState::CANCELED;
    }
    if (raw == "REJECTED" || raw == "FAILED") {
        return OrderState::REJECTED;
    }
    return OrderState::PENDING;
}

auto parse_coinbase_query(const std::string& body, FillUpdate& status) -> bool {
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded()) {
        return false;
    }
    const auto& order = json["order"];
    if (!order.is_object()) {
        return false;
    }

    status.cumulative_filled_qty = order.value("filled_size", 0.0);
    status.fill_qty = status.cumulative_filled_qty;
    status.avg_fill_price = order.value("average_filled_price", 0.0);
    status.fill_price = status.avg_fill_price;
    status.new_state = parse_coinbase_status(order.value("status", std::string("")));
    status.local_ts_ns = http::now_ns();
    return true;
}

auto parse_coinbase_cancel_ack(const std::string& body) -> bool {
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded()) {
        return false;
    }
    const auto& results = json["results"];
    if (!results.is_array() || results.empty()) {
        return false;
    }
    return results[0].value("success", false);
}

auto append_coinbase_open_orders(const std::string& body, ReconciliationSnapshot& snapshot)
    -> ConnectorResult {
    const auto open_json = nlohmann::json::parse(body, nullptr, false);
    const auto& orders = open_json["orders"];
    if (!orders.is_array()) {
        return ConnectorResult::ERROR_UNKNOWN;
    }

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
        if (!snapshot.open_orders.push(order)) {
            return ConnectorResult::ERROR_UNKNOWN;
        }
    }

    return ConnectorResult::OK;
}

auto append_coinbase_balances(const std::string& body, ReconciliationSnapshot& snapshot)
    -> ConnectorResult {
    const auto account_json = nlohmann::json::parse(body, nullptr, false);
    const auto& accounts = account_json["accounts"];
    if (!accounts.is_array()) {
        return ConnectorResult::ERROR_UNKNOWN;
    }

    for (const auto& item : accounts) {
        ReconciledBalance balance;
        copy_cstr(balance.asset, sizeof(balance.asset), item.value("currency", std::string("")));
        balance.total =
            item["available_balance"].value("value", 0.0) + item["hold"].value("value", 0.0);
        balance.available = item["available_balance"].value("value", 0.0);
        if (!snapshot.balances.push(balance)) {
            return ConnectorResult::ERROR_UNKNOWN;
        }
    }

    return ConnectorResult::OK;
}

auto append_coinbase_positions(const std::string& body, ReconciliationSnapshot& snapshot)
    -> ConnectorResult {
    const auto pos_json = nlohmann::json::parse(body, nullptr, false);
    const auto& positions = pos_json["positions"];
    if (!positions.is_array()) {
        return ConnectorResult::ERROR_UNKNOWN;
    }

    for (const auto& item : positions) {
        ReconciledPosition position;
        copy_cstr(position.symbol, sizeof(position.symbol),
                  item.value("product_id", std::string("")));
        position.quantity = item.value("size", 0.0);
        position.avg_entry_price = item.value("average_entry_price", 0.0);
        if (!snapshot.positions.push(position)) {
            return ConnectorResult::ERROR_UNKNOWN;
        }
    }

    return ConnectorResult::OK;
}

auto append_coinbase_fills(const std::string& body, ReconciliationSnapshot& snapshot)
    -> ConnectorResult {
    const auto fills_json = nlohmann::json::parse(body, nullptr, false);
    const auto& fills = fills_json["fills"];
    if (!fills.is_array()) {
        return ConnectorResult::ERROR_UNKNOWN;
    }

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
        if (!snapshot.fills.push(fill)) {
            return ConnectorResult::ERROR_UNKNOWN;
        }
    }

    return ConnectorResult::OK;
}

} // namespace

auto CoinbaseConnector::submit_to_venue(const Order& order, const std::string& idempotency_key,
                                        std::string& venue_order_id) -> ConnectorResult {
    const std::string payload = order_payload(order);
    const auto headers = auth_headers("POST", "/api/v3/brokerage/orders", payload, idempotency_key);
    const auto resp = http::post(api_url() + "/api/v3/brokerage/orders", payload, headers);
    if (!resp.ok()) {
        return classify_error(resp.status);
    }

    if (!parse_coinbase_order_id(resp.body, venue_order_id)) {
        return ConnectorResult::ERROR_UNKNOWN;
    }
    return ConnectorResult::OK;
}

auto CoinbaseConnector::cancel_at_venue(const VenueOrderEntry& entry) -> ConnectorResult {
    const std::string payload = std::string(R"({"order_ids":[")") + entry.venue_order_id + R"("]})";
    const auto resp =
        http::post(api_url() + "/api/v3/brokerage/orders/batch_cancel", payload,
                   auth_headers("POST", "/api/v3/brokerage/orders/batch_cancel", payload));
    if (!resp.ok()) {
        return classify_error(resp.status);
    }
    return parse_coinbase_cancel_ack(resp.body) ? ConnectorResult::OK
                                                : ConnectorResult::ERROR_UNKNOWN;
}

auto CoinbaseConnector::replace_at_venue(const VenueOrderEntry& entry, const Order& replacement,
                                         std::string& new_venue_order_id) -> ConnectorResult {
    const std::string payload = std::string(R"({"order_id":")") + entry.venue_order_id +
                                R"(","size":")" + std::to_string(replacement.quantity) +
                                R"(","limit_price":")" + std::to_string(replacement.price) +
                                R"("})";
    const auto resp = http::post(api_url() + "/api/v3/brokerage/orders/edit", payload,
                                 auth_headers("POST", "/api/v3/brokerage/orders/edit", payload));
    if (!resp.ok()) {
        return classify_error(resp.status);
    }
    return parse_coinbase_order_id(resp.body, new_venue_order_id) ? ConnectorResult::OK
                                                                  : ConnectorResult::ERROR_UNKNOWN;
}

auto CoinbaseConnector::query_at_venue(const VenueOrderEntry& entry, FillUpdate& status)
    -> ConnectorResult {
    const auto resp = http::get(
        api_url() + "/api/v3/brokerage/orders/historical/" + std::string(entry.venue_order_id),
        auth_headers(
            "GET", std::string("/api/v3/brokerage/orders/historical/") + entry.venue_order_id, ""));
    if (!resp.ok()) {
        return classify_error(resp.status);
    }
    return parse_coinbase_query(resp.body, status) ? ConnectorResult::OK
                                                   : ConnectorResult::ERROR_UNKNOWN;
}

auto CoinbaseConnector::cancel_all_at_venue(const char* symbol) -> ConnectorResult {
    const std::string payload =
        std::string(R"({"product_id":")") + ((symbol != nullptr) ? symbol : "") + R"("})";
    const auto resp =
        http::post(api_url() + "/api/v3/brokerage/orders/batch_cancel", payload,
                   auth_headers("POST", "/api/v3/brokerage/orders/batch_cancel", payload));
    if (!resp.ok()) {
        return classify_error(resp.status);
    }
    return parse_coinbase_cancel_ack(resp.body) ? ConnectorResult::OK
                                                : ConnectorResult::ERROR_UNKNOWN;
}

} // namespace trading

namespace trading {

auto CoinbaseConnector::fetch_reconciliation_snapshot(ReconciliationSnapshot& snapshot)
    -> ConnectorResult {
    snapshot.clear();

    const auto open_orders =
        http::get(api_url() + "/api/v3/brokerage/orders/historical/batch",
                  auth_headers("GET", "/api/v3/brokerage/orders/historical/batch", ""));
    if (!open_orders.ok()) {
        return classify_error(open_orders.status);
    }

    ConnectorResult result = append_coinbase_open_orders(open_orders.body, snapshot);
    if (result != ConnectorResult::OK) {
        return result;
    }

    const auto account_resp = http::get(api_url() + "/api/v3/brokerage/accounts",
                                        auth_headers("GET", "/api/v3/brokerage/accounts", ""));
    if (!account_resp.ok()) {
        return classify_error(account_resp.status);
    }
    result = append_coinbase_balances(account_resp.body, snapshot);
    if (result != ConnectorResult::OK) {
        return result;
    }

    const auto pos_resp = http::get(api_url() + "/api/v3/brokerage/positions",
                                    auth_headers("GET", "/api/v3/brokerage/positions", ""));
    if (!pos_resp.ok()) {
        return classify_error(pos_resp.status);
    }
    result = append_coinbase_positions(pos_resp.body, snapshot);
    if (result != ConnectorResult::OK) {
        return result;
    }

    const auto fills_resp =
        http::get(api_url() + "/api/v3/brokerage/orders/historical/fills",
                  auth_headers("GET", "/api/v3/brokerage/orders/historical/fills", ""));
    if (fills_resp.status == 404 || fills_resp.status == 405 || fills_resp.status == 501) {
        return ConnectorResult::OK;
    }
    if (!fills_resp.ok()) {
        return classify_error(fills_resp.status);
    }
    return append_coinbase_fills(fills_resp.body, snapshot);
}

} // namespace trading
