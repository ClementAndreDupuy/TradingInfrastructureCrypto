#include "okx_connector.hpp"

#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
#else
#include "../../common/json.hpp"
#endif

#include <cmath>
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
    const auto resp =
        http::post(api_url() + "/api/v5/trade/cancel-order", payload, auth_headers(payload));
    if (!resp.ok())
        return classify_error(resp.status);
    return parse_okx_cancel_ack(resp.body) ? ConnectorResult::OK : ConnectorResult::ERROR_UNKNOWN;
}

ConnectorResult OkxConnector::replace_at_venue(const VenueOrderEntry& entry,
                                               const Order& replacement,
                                               std::string& new_venue_order_id) {
    const std::string payload = std::string("{\"ordId\":\"") + entry.venue_order_id +
                                "\",\"newPx\":\"" + std::to_string(replacement.price) +
                                "\",\"newSz\":\"" + std::to_string(replacement.quantity) + "\"}";
    const auto resp =
        http::post(api_url() + "/api/v5/trade/amend-order", payload, auth_headers(payload));
    if (!resp.ok())
        return classify_error(resp.status);
    return parse_okx_order_id(resp.body, new_venue_order_id) ? ConnectorResult::OK
                                                             : ConnectorResult::ERROR_UNKNOWN;
}

ConnectorResult OkxConnector::query_at_venue(const VenueOrderEntry& entry, FillUpdate& status) {
    const auto resp =
        http::get(api_url() + "/api/v5/trade/order?ordId=" + std::string(entry.venue_order_id),
                  auth_headers(std::string("ordId=") + entry.venue_order_id));
    if (!resp.ok())
        return classify_error(resp.status);
    return parse_okx_query(resp.body, status) ? ConnectorResult::OK
                                              : ConnectorResult::ERROR_UNKNOWN;
}

ConnectorResult OkxConnector::cancel_all_at_venue(const char* symbol) {
    const std::string payload = std::string("{\"instId\":\"") + (symbol ? symbol : "") + "\"}";
    const auto resp =
        http::post(api_url() + "/api/v5/trade/cancel-batch-orders", payload, auth_headers(payload));
    if (!resp.ok())
        return classify_error(resp.status);
    return parse_okx_cancel_ack(resp.body) ? ConnectorResult::OK : ConnectorResult::ERROR_UNKNOWN;
}

} // namespace trading

namespace trading {

ConnectorResult OkxConnector::fetch_reconciliation_snapshot(ReconciliationSnapshot& snapshot) {
    snapshot.clear();

    const auto open_orders =
        http::get(api_url() + "/api/v5/trade/orders-pending", auth_headers("orders-pending"));
    if (!open_orders.ok())
        return classify_error(open_orders.status);

    const auto order_json = nlohmann::json::parse(open_orders.body, nullptr, false);
    const auto& orders = order_json["data"];
    if (!orders.is_array())
        return ConnectorResult::ERROR_UNKNOWN;

    for (const auto& item : orders) {
        ReconciledOrder order;
        copy_cstr(order.venue_order_id, sizeof(order.venue_order_id),
                  item.value("ordId", std::string("")));
        copy_cstr(order.symbol, sizeof(order.symbol), item.value("instId", std::string("")));
        order.side = item.value("side", std::string("buy")) == "sell" ? Side::ASK : Side::BID;
        order.quantity = item.value("sz", 0.0);
        order.filled_quantity = item.value("accFillSz", 0.0);
        order.price = item.value("px", 0.0);
        order.state = parse_okx_status(item.value("state", std::string("")));
        if (!snapshot.open_orders.push(order))
            return ConnectorResult::ERROR_UNKNOWN;
    }

    const auto account = http::get(api_url() + "/api/v5/account/balance", auth_headers("balance"));
    if (!account.ok())
        return classify_error(account.status);

    const auto account_json = nlohmann::json::parse(account.body, nullptr, false);
    const auto& details = account_json["data"][0]["details"];
    if (!details.is_array())
        return ConnectorResult::ERROR_UNKNOWN;

    for (const auto& item : details) {
        ReconciledBalance balance;
        copy_cstr(balance.asset, sizeof(balance.asset), item.value("ccy", std::string("")));
        balance.total = item.value("eq", 0.0);
        balance.available = item.value("availEq", 0.0);
        if (!snapshot.balances.push(balance))
            return ConnectorResult::ERROR_UNKNOWN;
    }

    const auto pos_resp =
        http::get(api_url() + "/api/v5/account/positions", auth_headers("positions"));
    if (!pos_resp.ok())
        return classify_error(pos_resp.status);

    const auto pos_json = nlohmann::json::parse(pos_resp.body, nullptr, false);
    const auto& positions = pos_json["data"];
    if (!positions.is_array())
        return ConnectorResult::ERROR_UNKNOWN;

    for (const auto& item : positions) {
        ReconciledPosition position;
        copy_cstr(position.symbol, sizeof(position.symbol), item.value("instId", std::string("")));
        position.quantity = item.value("pos", 0.0);
        position.avg_entry_price = item.value("avgPx", 0.0);
        if (!snapshot.positions.push(position))
            return ConnectorResult::ERROR_UNKNOWN;
    }

    const auto fills_resp =
        http::get(api_url() + "/api/v5/trade/fills", auth_headers("trade/fills"));
    if (fills_resp.ok()) {
        const auto fills_json = nlohmann::json::parse(fills_resp.body, nullptr, false);
        const auto& fills = fills_json["data"];
        if (!fills.is_array())
            return ConnectorResult::ERROR_UNKNOWN;

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
            if (!snapshot.fills.push(fill))
                return ConnectorResult::ERROR_UNKNOWN;
        }
    }

    return ConnectorResult::OK;
}

} // namespace trading
