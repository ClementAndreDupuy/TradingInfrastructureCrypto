#include "okx_connector.hpp"

#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
#else
#include "../../common/json.hpp"
#endif

#include "../../common/symbol_mapper.hpp"

#include <cmath>
#include <sstream>
#include <string>

namespace trading {
namespace {

auto format_number(double value) -> std::string {
    std::ostringstream oss;
    oss.precision(15);
    oss << value;
    return oss.str();
}

auto okx_client_order_id(uint64_t client_order_id) -> std::string {
    return std::string("TRT-") + std::to_string(client_order_id) + "-OKX";
}

auto okx_trade_mode(const std::string& inst_id) -> const char* {
    return inst_id.find("-SWAP") != std::string::npos ||
                   inst_id.find("-FUTURES") != std::string::npos ||
                   inst_id.find("-OPTION") != std::string::npos
               ? "cross"
               : "cash";
}

auto okx_order_type(const Order& order) -> const char* {
    if (order.type == OrderType::MARKET) {
        return "market";
    }
    switch (order.tif) {
    case TimeInForce::IOC:
        return "ioc";
    case TimeInForce::FOK:
        return "fok";
    case TimeInForce::GTX:
        return "post_only";
    case TimeInForce::GTC:
    default:
        return "limit";
    }
}

auto build_order_payload(const Order& order) -> std::string {
    if (order.type == OrderType::STOP_LIMIT) {
        return {};
    }
    const std::string inst_id = SymbolMapper::map_for_exchange(Exchange::OKX, order.symbol);
    std::string payload = std::string(R"({"instId":")") + inst_id + R"(","tdMode":")" +
                          okx_trade_mode(inst_id) + R"(","clOrdId":")" +
                          okx_client_order_id(order.client_order_id) + R"(","side":")" +
                          (order.side == Side::BID ? "buy" : "sell") + R"(","ordType":")" +
                          okx_order_type(order) + R"(","sz":")" + format_number(order.quantity) +
                          "\"";
    if (order.type != OrderType::MARKET) {
        payload += R"(,"px":")" + format_number(order.price) + "\"";
    }
    payload += "}";
    return payload;
}

auto parse_order_id(const std::string& body, std::string& venue_order_id) -> bool {
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded()) {
        return false;
    }
    const auto& data = json["data"];
    if (!data.is_array() || data.empty()) {
        return false;
    }
    venue_order_id = data[0].value("ordId", std::string(""));
    return !venue_order_id.empty();
}

auto parse_order_status(const std::string& raw) -> OrderState {
    if (raw == "live") {
        return OrderState::OPEN;
    }
    if (raw == "partially_filled") {
        return OrderState::PARTIALLY_FILLED;
    }
    if (raw == "filled") {
        return OrderState::FILLED;
    }
    if (raw == "canceled") {
        return OrderState::CANCELED;
    }
    return OrderState::REJECTED;
}

auto parse_query_status(const std::string& body, FillUpdate& status) -> bool {
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded()) {
        return false;
    }
    const auto& data = json["data"];
    if (!data.is_array() || data.empty()) {
        return false;
    }
    const auto& first = data[0];
    status.cumulative_filled_qty = first.value("accFillSz", 0.0);
    status.fill_qty = status.cumulative_filled_qty;
    status.avg_fill_price = first.value("avgPx", 0.0);
    status.fill_price = status.avg_fill_price;
    status.new_state = parse_order_status(first.value("state", std::string("")));
    status.local_ts_ns = http::now_ns();
    return true;
}

auto parse_cancel_result(const std::string& body) -> bool {
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded()) {
        return false;
    }
    const auto& data = json["data"];
    if (!data.is_array() || data.empty()) {
        return false;
    }
    return data[0].value("sCode", std::string("")) == "0";
}

auto append_open_orders(const std::string& body,
                            ReconciliationSnapshot& snapshot) -> ConnectorResult {
    const auto order_json = nlohmann::json::parse(body, nullptr, false);
    const auto& orders = order_json["data"];
    if (!orders.is_array()) {
        return ConnectorResult::ERROR_UNKNOWN;
    }

    for (const auto& item : orders) {
        ReconciledOrder order;
        copy_cstr(order.venue_order_id, sizeof(order.venue_order_id),
                  item.value("ordId", std::string("")));
        copy_cstr(order.symbol, sizeof(order.symbol), item.value("instId", std::string("")));
        order.side = item.value("side", std::string("buy")) == "sell" ? Side::ASK : Side::BID;
        order.quantity = item.value("sz", 0.0);
        order.filled_quantity = item.value("accFillSz", 0.0);
        order.price = item.value("px", 0.0);
        order.state = parse_order_status(item.value("state", std::string("")));
        if (!snapshot.open_orders.push(order)) {
            return ConnectorResult::ERROR_UNKNOWN;
        }
    }

    return ConnectorResult::OK;
}

auto append_balances(const std::string& body,
                         ReconciliationSnapshot& snapshot) -> ConnectorResult {
    const auto account_json = nlohmann::json::parse(body, nullptr, false);
    const auto& details = account_json["data"][0]["details"];
    if (!details.is_array()) {
        return ConnectorResult::ERROR_UNKNOWN;
    }

    for (const auto& item : details) {
        ReconciledBalance balance;
        copy_cstr(balance.asset, sizeof(balance.asset), item.value("ccy", std::string("")));
        balance.total = item.value("eq", 0.0);
        balance.available = item.value("availEq", 0.0);
        if (!snapshot.balances.push(balance)) {
            return ConnectorResult::ERROR_UNKNOWN;
        }
    }

    return ConnectorResult::OK;
}

auto append_positions(const std::string& body,
                          ReconciliationSnapshot& snapshot) -> ConnectorResult {
    const auto pos_json = nlohmann::json::parse(body, nullptr, false);
    const auto& positions = pos_json["data"];
    if (!positions.is_array()) {
        return ConnectorResult::ERROR_UNKNOWN;
    }

    for (const auto& item : positions) {
        ReconciledPosition position;
        copy_cstr(position.symbol, sizeof(position.symbol), item.value("instId", std::string("")));
        position.quantity = item.value("pos", 0.0);
        position.avg_entry_price = item.value("avgPx", 0.0);
        if (!snapshot.positions.push(position)) {
            return ConnectorResult::ERROR_UNKNOWN;
        }
    }

    return ConnectorResult::OK;
}

auto append_fills(const std::string& body,
                      ReconciliationSnapshot& snapshot) -> ConnectorResult {
    const auto fills_json = nlohmann::json::parse(body, nullptr, false);
    const auto& fills = fills_json["data"];
    if (!fills.is_array()) {
        return ConnectorResult::ERROR_UNKNOWN;
    }

    for (const auto& item : fills) {
        ReconciledFill fill;
        fill.exchange = Exchange::OKX;
        copy_cstr(fill.venue_trade_id, sizeof(fill.venue_trade_id),
                  item.value("tradeId", std::string("")));
        copy_cstr(fill.venue_order_id, sizeof(fill.venue_order_id),
                  item.value("ordId", std::string("")));
        copy_cstr(fill.symbol, sizeof(fill.symbol), item.value("instId", std::string("")));
        fill.side = item.value("side", std::string("buy")) == "sell" ? Side::ASK : Side::BID;
        fill.quantity = item.value("fillSz", 0.0);
        fill.price = item.value("fillPx", 0.0);
        fill.notional = fill.quantity * fill.price;
        fill.fee = std::fabs(item.value("fee", 0.0));
        copy_cstr(fill.fee_asset, sizeof(fill.fee_asset), item.value("feeCcy", std::string("")));
        fill.exchange_ts_ns = item.value("ts", static_cast<int64_t>(0)) * 1000000;
        if (!snapshot.fills.push(fill)) {
            return ConnectorResult::ERROR_UNKNOWN;
        }
    }

    return ConnectorResult::OK;
}

}

auto OkxConnector::submit_to_venue(const Order& order, const std::string& idempotency_key,
                                   std::string& venue_order_id) -> ConnectorResult {
    const std::string payload = build_order_payload(order);
    if (payload.empty()) {
        return ConnectorResult::ERROR_INVALID_ORDER;
    }
    const auto headers = auth_headers("POST", "/api/v5/trade/order", payload, idempotency_key);
    const auto resp = http::post(api_url() + "/api/v5/trade/order", payload, headers);
    if (!resp.ok()) {
        return classify_http_error(resp.status);
    }

    if (!parse_order_id(resp.body, venue_order_id)) {
        return ConnectorResult::ERROR_UNKNOWN;
    }
    return ConnectorResult::OK;
}

auto OkxConnector::cancel_at_venue(const VenueOrderEntry& entry) -> ConnectorResult {
    const std::string inst_id = SymbolMapper::map_for_exchange(Exchange::OKX, entry.symbol);
    const std::string payload = std::string(R"({"instId":")") + inst_id + R"(","ordId":")" +
                                entry.venue_order_id + R"(","clOrdId":")" +
                                okx_client_order_id(entry.client_order_id) + R"("})";
    const auto resp = http::post(api_url() + "/api/v5/trade/cancel-order", payload,
                                 auth_headers("POST", "/api/v5/trade/cancel-order", payload));
    if (!resp.ok()) {
        return classify_http_error(resp.status);
    }
    return parse_cancel_result(resp.body) ? ConnectorResult::OK : ConnectorResult::ERROR_UNKNOWN;
}

auto OkxConnector::replace_at_venue(const VenueOrderEntry& entry, const Order& replacement,
                                    std::string& new_venue_order_id) -> ConnectorResult {
    if (replacement.type == OrderType::STOP_LIMIT) {
        return ConnectorResult::ERROR_INVALID_ORDER;
    }
    const std::string inst_id = SymbolMapper::map_for_exchange(Exchange::OKX, entry.symbol);
    const std::string payload = std::string(R"({"instId":")") + inst_id + R"(","ordId":")" +
                                entry.venue_order_id + R"(","clOrdId":")" +
                                okx_client_order_id(entry.client_order_id) + R"(","newSz":")" +
                                format_number(replacement.quantity) + R"(","newPx":")" +
                                format_number(replacement.price) + R"(","reqId":")" +
                                okx_client_order_id(replacement.client_order_id) + R"("})";
    const auto resp = http::post(api_url() + "/api/v5/trade/amend-order", payload,
                                 auth_headers("POST", "/api/v5/trade/amend-order", payload));
    if (!resp.ok()) {
        return classify_http_error(resp.status);
    }
    return parse_order_id(resp.body, new_venue_order_id) ? ConnectorResult::OK
                                                             : ConnectorResult::ERROR_UNKNOWN;
}

auto OkxConnector::query_at_venue(const VenueOrderEntry& entry,
                                  FillUpdate& status) -> ConnectorResult {
    const std::string inst_id = SymbolMapper::map_for_exchange(Exchange::OKX, entry.symbol);
    const std::string query = std::string("instId=") + inst_id + "&ordId=" + entry.venue_order_id +
                              "&clOrdId=" + okx_client_order_id(entry.client_order_id);
    const auto resp = http::get(api_url() + "/api/v5/trade/order?" + query,
                                auth_headers("GET", std::string("/api/v5/trade/order?") + query, ""));
    if (!resp.ok()) {
        return classify_http_error(resp.status);
    }
    return parse_query_status(resp.body, status) ? ConnectorResult::OK
                                              : ConnectorResult::ERROR_UNKNOWN;
}

auto OkxConnector::cancel_all_at_venue(const char* symbol) -> ConnectorResult {
    (void)symbol;
    return ConnectorResult::ERROR_INVALID_ORDER;
}

}

namespace trading {

auto OkxConnector::fetch_reconciliation_snapshot(ReconciliationSnapshot& snapshot)
    -> ConnectorResult {
    snapshot.clear();

    const auto open_orders = http::get(api_url() + "/api/v5/trade/orders-pending",
                                       auth_headers("GET", "/api/v5/trade/orders-pending", ""));
    if (!open_orders.ok()) {
        return classify_http_error(open_orders.status);
    }

    ConnectorResult result = append_open_orders(open_orders.body, snapshot);
    if (result != ConnectorResult::OK) {
        return result;
    }

    const auto account = http::get(api_url() + "/api/v5/account/balance",
                                   auth_headers("GET", "/api/v5/account/balance", ""));
    if (!account.ok()) {
        return classify_http_error(account.status);
    }
    result = append_balances(account.body, snapshot);
    if (result != ConnectorResult::OK) {
        return result;
    }

    const auto pos_resp = http::get(api_url() + "/api/v5/account/positions",
                                    auth_headers("GET", "/api/v5/account/positions", ""));
    if (!pos_resp.ok()) {
        return classify_http_error(pos_resp.status);
    }
    result = append_positions(pos_resp.body, snapshot);
    if (result != ConnectorResult::OK) {
        return result;
    }

    const auto fills_resp = http::get(api_url() + "/api/v5/trade/fills",
                                      auth_headers("GET", "/api/v5/trade/fills", ""));
    if (fills_resp.status == 404 || fills_resp.status == 405 || fills_resp.status == 501) {
        return ConnectorResult::OK;
    }
    if (!fills_resp.ok()) {
        return classify_http_error(fills_resp.status);
    }
    return append_fills(fills_resp.body, snapshot);
}

}
