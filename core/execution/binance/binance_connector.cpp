#include "binance_connector.hpp"

#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
#else
#include "../../common/json.hpp"
#endif

#include <string>

namespace trading {
namespace {

auto order_payload(const Order& order) -> std::string {
    return std::string("{\"symbol\":\"") + order.symbol + "\",\"side\":\"" +
           (order.side == Side::BID ? "BUY" : "SELL") + "\",\"type\":\"" +
           (order.type == OrderType::MARKET ? "MARKET" : "LIMIT") +
           "\",\"quantity\":" + std::to_string(order.quantity) +
           ",\"price\":" + std::to_string(order.price) + "}";
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

auto parse_binance_order_id(const std::string& body, std::string& venue_order_id) -> bool {
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded()) {
        return false;
    }

    const auto raw_id = json.value("orderId", std::string(""));
    if (!raw_id.empty()) {
        venue_order_id = raw_id;
        return true;
    }

    const uint64_t numeric_id = json.value("orderId", uint64_t{0});
    if (numeric_id != 0) {
        venue_order_id = std::to_string(numeric_id);
        return true;
    }
    return false;
}

auto parse_binance_status(const std::string& raw) -> OrderState {
    if (raw == "NEW" || raw == "PARTIALLY_FILLED") {
        return raw == "NEW" ? OrderState::OPEN : OrderState::PARTIALLY_FILLED;
    }
    if (raw == "FILLED") {
        return OrderState::FILLED;
    }
    if (raw == "CANCELED" || raw == "PENDING_CANCEL") {
        return OrderState::CANCELED;
    }
    if (raw == "REJECTED" || raw == "EXPIRED") {
        return OrderState::REJECTED;
    }
    return OrderState::PENDING;
}

auto parse_binance_query(const std::string& body, FillUpdate& status) -> bool {
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded()) {
        return false;
    }

    status.cumulative_filled_qty = json.value("executedQty", 0.0);
    status.fill_qty = status.cumulative_filled_qty;
    status.avg_fill_price = json.value("avgPrice", 0.0);
    status.fill_price = status.avg_fill_price;
    status.new_state = parse_binance_status(json.value("status", std::string("")));
    status.local_ts_ns = http::now_ns();
    return true;
}

auto parse_binance_cancel_ack(const std::string& body) -> bool {
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded()) {
        return false;
    }
    if (json.find("orderId") != json.end()) {
        return true;
    }
    return json.value("status", std::string("")) == "CANCELED";
}

} // namespace

auto BinanceConnector::submit_to_venue(const Order& order, const std::string& idempotency_key,
                                       std::string& venue_order_id) -> ConnectorResult {
    const std::string payload = order_payload(order);
    const auto headers = auth_headers("POST", "/api/v3/order", payload, idempotency_key);
    const auto resp = http::post(api_url() + "/api/v3/order", payload, headers);
    if (!resp.ok()) {
        return classify_error(resp.status);
    }

    if (!parse_binance_order_id(resp.body, venue_order_id)) {
        return ConnectorResult::ERROR_UNKNOWN;
    }
    return ConnectorResult::OK;
}

auto BinanceConnector::cancel_at_venue(const VenueOrderEntry& entry) -> ConnectorResult {
    const auto resp = http::del(
        api_url() + "/api/v3/order?orderId=" + std::string(entry.venue_order_id),
        auth_headers("DELETE", std::string("/api/v3/order?orderId=") + entry.venue_order_id, ""));
    if (!resp.ok()) {
        return classify_error(resp.status);
    }
    return parse_binance_cancel_ack(resp.body) ? ConnectorResult::OK
                                               : ConnectorResult::ERROR_UNKNOWN;
}

auto BinanceConnector::replace_at_venue(const VenueOrderEntry& entry, const Order& replacement,
                                        std::string& new_venue_order_id) -> ConnectorResult {
    const std::string payload = order_payload(replacement);
    const auto resp = http::put(
        api_url() + "/api/v3/order?orderId=" + std::string(entry.venue_order_id), payload,
        auth_headers("PUT", std::string("/api/v3/order?orderId=") + entry.venue_order_id, payload));
    if (!resp.ok()) {
        return classify_error(resp.status);
    }

    if (!parse_binance_order_id(resp.body, new_venue_order_id)) {
        return ConnectorResult::ERROR_UNKNOWN;
    }
    return ConnectorResult::OK;
}

auto BinanceConnector::query_at_venue(const VenueOrderEntry& entry, FillUpdate& status)
    -> ConnectorResult {
    const auto resp = http::get(
        api_url() + "/api/v3/order?orderId=" + std::string(entry.venue_order_id),
        auth_headers("GET", std::string("/api/v3/order?orderId=") + entry.venue_order_id, ""));
    if (!resp.ok()) {
        return classify_error(resp.status);
    }
    return parse_binance_query(resp.body, status) ? ConnectorResult::OK
                                                  : ConnectorResult::ERROR_UNKNOWN;
}

auto BinanceConnector::cancel_all_at_venue(const char* symbol) -> ConnectorResult {
    const auto resp = http::del(
        api_url() + "/api/v3/openOrders?symbol=" + std::string((symbol != nullptr) ? symbol : ""),
        auth_headers(
            "DELETE",
            std::string("/api/v3/openOrders?symbol=") + ((symbol != nullptr) ? symbol : ""), ""));
    if (!resp.ok()) {
        return classify_error(resp.status);
    }
    const auto json = nlohmann::json::parse(resp.body, nullptr, false);
    return (!json.is_discarded() && json.is_array()) ? ConnectorResult::OK
                                                     : ConnectorResult::ERROR_UNKNOWN;
}

} // namespace trading

namespace trading {

auto BinanceConnector::fetch_reconciliation_snapshot(ReconciliationSnapshot& snapshot)
    -> ConnectorResult {
    snapshot.clear();

    const auto open_orders =
        http::get(api_url() + "/api/v3/openOrders", auth_headers("GET", "/api/v3/openOrders", ""));
    if (!open_orders.ok()) {
        return classify_error(open_orders.status);
    }

    const auto order_json = nlohmann::json::parse(open_orders.body, nullptr, false);
    if (!order_json.is_array()) {
        return ConnectorResult::ERROR_UNKNOWN;
    }

    for (const auto& item : order_json) {
        ReconciledOrder order;
        order.client_order_id = item.value("clientOrderId", uint64_t{0});
        copy_cstr(order.venue_order_id, sizeof(order.venue_order_id),
                  item.value("orderId", std::string("")));
        copy_cstr(order.symbol, sizeof(order.symbol), item.value("symbol", std::string("")));
        order.side = item.value("side", std::string("BUY")) == "SELL" ? Side::ASK : Side::BID;
        order.quantity = item.value("origQty", 0.0);
        order.filled_quantity = item.value("executedQty", 0.0);
        order.price = item.value("price", 0.0);
        order.state = parse_binance_status(item.value("status", std::string("")));
        if (!snapshot.open_orders.push(order)) {
            return ConnectorResult::ERROR_UNKNOWN;
        }
    }

    const auto account =
        http::get(api_url() + "/api/v3/account", auth_headers("GET", "/api/v3/account", ""));
    if (!account.ok()) {
        return classify_error(account.status);
    }

    const auto account_json = nlohmann::json::parse(account.body, nullptr, false);
    const auto& balances = account_json["balances"];
    if (!balances.is_array()) {
        return ConnectorResult::ERROR_UNKNOWN;
    }

    for (const auto& item : balances) {
        ReconciledBalance balance;
        copy_cstr(balance.asset, sizeof(balance.asset), item.value("asset", std::string("")));
        balance.total = item.value("free", 0.0) + item.value("locked", 0.0);
        balance.available = item.value("free", 0.0);
        if (!snapshot.balances.push(balance)) {
            return ConnectorResult::ERROR_UNKNOWN;
        }
    }

    const auto trades = http::get(api_url() + "/api/v3/myTrades",
                                  auth_headers("GET", "/api/v3/myTrades", "", "recon-trades"));
    if (trades.status == 404 || trades.status == 405 || trades.status == 501) {
        return ConnectorResult::OK;
    }
    if (!trades.ok()) {
        return classify_error(trades.status);
    }

    const auto trades_json = nlohmann::json::parse(trades.body, nullptr, false);
    if (!trades_json.is_array()) {
        return ConnectorResult::ERROR_UNKNOWN;
    }
    for (const auto& item : trades_json) {
        ReconciledFill fill;
        fill.exchange = Exchange::BINANCE;
        fill.client_order_id = item.value("orderId", static_cast<uint64_t>(0));
        copy_cstr(fill.venue_order_id, sizeof(fill.venue_order_id),
                  std::to_string(item.value("orderId", static_cast<uint64_t>(0))));
        copy_cstr(fill.venue_trade_id, sizeof(fill.venue_trade_id),
                  std::to_string(item.value("id", static_cast<uint64_t>(0))));
        copy_cstr(fill.symbol, sizeof(fill.symbol), item.value("symbol", std::string("")));
        fill.side = item.value("isBuyer", true) ? Side::BID : Side::ASK;
        fill.quantity = item.value("qty", 0.0);
        fill.price = item.value("price", 0.0);
        fill.notional = fill.quantity * fill.price;
        fill.fee = item.value("commission", 0.0);
        copy_cstr(fill.fee_asset, sizeof(fill.fee_asset),
                  item.value("commissionAsset", std::string("")));
        fill.exchange_ts_ns = item.value("time", static_cast<int64_t>(0)) * 1000000;
        if (!snapshot.fills.push(fill)) {
            return ConnectorResult::ERROR_UNKNOWN;
        }
    }

    return ConnectorResult::OK;
}

} // namespace trading
