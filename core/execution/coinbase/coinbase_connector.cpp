#include "coinbase_connector.hpp"

#include <nlohmann/json.hpp>

#include "../../common/symbol_mapper.hpp"

#include <string>

namespace trading {
    namespace {
        auto decimal_string(double value) -> std::string {
            std::string out = std::to_string(value);
            while (!out.empty() && out.back() == '0') {
                out.pop_back();
            }
            if (!out.empty() && out.back() == '.') {
                out.pop_back();
            }
            return out.empty() ? std::string("0") : out;
        }

        auto build_order_payload(const Order &order) -> std::string {
            const std::string product_id = SymbolMapper::map_for_exchange(Exchange::COINBASE, order.symbol);
            std::string config;
            if (order.type == OrderType::MARKET) {
                config = std::string(R"("market_market_ioc":{"base_size":")") +
                         decimal_string(order.quantity) + R"("})";
            } else if (order.type == OrderType::LIMIT) {
                if (order.tif == TimeInForce::IOC) {
                    config = std::string(R"("sor_limit_ioc":{"base_size":")") +
                             decimal_string(order.quantity) + R"(","limit_price":")" +
                             decimal_string(order.price) + R"("})";
                } else if (order.tif == TimeInForce::FOK) {
                    config = std::string(R"("limit_limit_fok":{"base_size":")") +
                             decimal_string(order.quantity) + R"(","limit_price":")" +
                             decimal_string(order.price) + R"("})";
                } else {
                    config = std::string(R"("limit_limit_gtc":{"base_size":")") +
                             decimal_string(order.quantity) + R"(","limit_price":")" +
                             decimal_string(order.price) + R"(","post_only":false})";
                }
            }
            return std::string(R"({"client_order_id":")") + std::to_string(order.client_order_id) +
                   R"(","product_id":")" + product_id + R"(","side":")" +
                   (order.side == Side::BID ? "BUY" : "SELL") + R"(","order_configuration":{)" +
                   config + "}}";
        }

        auto parse_order_id(const std::string &body, std::string &venue_order_id) -> bool {
            const auto json = nlohmann::json::parse(body, nullptr, false);
            if (json.is_discarded()) {
                return false;
            }
            if (json.value("success", false)) {
                venue_order_id = json["success_response"].value("order_id", std::string(""));
            } else if (json.find("success_response") != json.end()) {
                venue_order_id = json["success_response"].value("order_id", std::string(""));
            } else if (json.find("order_id") != json.end()) {
                venue_order_id = json.value("order_id", std::string(""));
            }
            return !venue_order_id.empty();
        }

        auto parse_order_status(const std::string &raw) -> OrderState {
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

        auto parse_query_status(const std::string &body, FillUpdate &status) -> bool {
            const auto json = nlohmann::json::parse(body, nullptr, false);
            if (json.is_discarded()) {
                return false;
            }
            const auto &order = json["order"];
            if (!order.is_object()) {
                return false;
            }

            status.cumulative_filled_qty = order.value("filled_size", 0.0);
            status.fill_qty = status.cumulative_filled_qty;
            status.avg_fill_price = order.value("average_filled_price", 0.0);
            status.fill_price = status.avg_fill_price;
            status.new_state = parse_order_status(order.value("status", std::string("")));
            status.local_ts_ns = http::now_ns();
            return true;
        }

        auto parse_cancel_result(const std::string &body) -> bool {
            const auto json = nlohmann::json::parse(body, nullptr, false);
            if (json.is_discarded()) {
                return false;
            }
            const auto &results = json["results"];
            if (!results.is_array() || results.empty()) {
                return false;
            }
            return results[0].value("success", false);
        }

        auto append_open_orders(const std::string &body,
                                ReconciliationSnapshot &snapshot) -> ConnectorResult {
            const auto open_json = nlohmann::json::parse(body, nullptr, false);
            const auto &orders = open_json["orders"];
            if (!orders.is_array()) {
                return ConnectorResult::ERROR_UNKNOWN;
            }

            for (const auto &item: orders) {
                ReconciledOrder order;
                copy_cstr(order.venue_order_id, sizeof(order.venue_order_id),
                          item.value("order_id", std::string("")));
                copy_cstr(order.symbol, sizeof(order.symbol), item.value("product_id", std::string("")));
                order.side = item.value("side", std::string("BUY")) == "SELL" ? Side::ASK : Side::BID;
                order.quantity = item.value("base_size", 0.0);
                order.filled_quantity = item.value("filled_size", 0.0);
                if (item.find("order_configuration") != item.end() && item["order_configuration"].is_object()) {
                    const auto &cfg = item["order_configuration"];
                    if (cfg.find("limit_limit_gtc") != cfg.end())
                        order.price = cfg["limit_limit_gtc"].value("limit_price", 0.0);
                    else if (cfg.find("sor_limit_ioc") != cfg.end())
                        order.price = cfg["sor_limit_ioc"].value("limit_price", 0.0);
                    else if (cfg.find("limit_limit_fok") != cfg.end())
                        order.price = cfg["limit_limit_fok"].value("limit_price", 0.0);
                }
                if (order.price <= 0.0)
                    order.price = item.value("limit_price", 0.0);
                order.state = parse_order_status(item.value("status", std::string("")));
                if (!snapshot.open_orders.push(order)) {
                    return ConnectorResult::ERROR_UNKNOWN;
                }
            }

            return ConnectorResult::OK;
        }

        auto append_balances(const std::string &body,
                             ReconciliationSnapshot &snapshot) -> ConnectorResult {
            const auto account_json = nlohmann::json::parse(body, nullptr, false);
            const auto &accounts = account_json["accounts"];
            if (!accounts.is_array()) {
                return ConnectorResult::ERROR_UNKNOWN;
            }

            for (const auto &item: accounts) {
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

        auto append_positions(const std::string &body,
                              ReconciliationSnapshot &snapshot) -> ConnectorResult {
            const auto pos_json = nlohmann::json::parse(body, nullptr, false);
            const auto &positions = pos_json["positions"];
            if (!positions.is_array()) {
                return ConnectorResult::ERROR_UNKNOWN;
            }

            for (const auto &item: positions) {
                ReconciledPosition position;
                copy_cstr(position.symbol, sizeof(position.symbol),
                          item.value("product_id", std::string("")));
                position.quantity = item.value("size", item.value("number_of_contracts", 0.0));
                position.avg_entry_price = item.value("average_entry_price", item.value("avg_entry_price", 0.0));
                if (!snapshot.positions.push(position)) {
                    return ConnectorResult::ERROR_UNKNOWN;
                }
            }

            return ConnectorResult::OK;
        }

        auto append_fills(const std::string &body,
                          ReconciliationSnapshot &snapshot) -> ConnectorResult {
            const auto fills_json = nlohmann::json::parse(body, nullptr, false);
            const auto &fills = fills_json["fills"];
            if (!fills.is_array()) {
                return ConnectorResult::ERROR_UNKNOWN;
            }

            for (const auto &item: fills) {
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
    }

    auto CoinbaseConnector::submit_to_venue(const Order &order, const std::string &idempotency_key,
                                            std::string &venue_order_id) -> ConnectorResult {
        if (order.type != OrderType::LIMIT && order.type != OrderType::MARKET) {
            return ConnectorResult::ERROR_INVALID_ORDER;
        }
        const std::string payload = build_order_payload(order);
        const auto headers = auth_headers("POST", "/api/v3/brokerage/orders", payload, idempotency_key);
        const auto resp = http::post(api_url() + "/api/v3/brokerage/orders", payload, headers);
        if (!resp.ok()) {
            return classify_http_error(resp.status);
        }

        if (!parse_order_id(resp.body, venue_order_id)) {
            return ConnectorResult::ERROR_UNKNOWN;
        }
        return ConnectorResult::OK;
    }

    auto CoinbaseConnector::cancel_at_venue(const VenueOrderEntry &entry) -> ConnectorResult {
        const std::string payload = std::string(R"({"order_ids":[")") + entry.venue_order_id + R"("]})";
        const auto resp =
                http::post(api_url() + "/api/v3/brokerage/orders/batch_cancel", payload,
                           auth_headers("POST", "/api/v3/brokerage/orders/batch_cancel", payload, ""));
        if (!resp.ok()) {
            return classify_http_error(resp.status);
        }
        return parse_cancel_result(resp.body)
                   ? ConnectorResult::OK
                   : ConnectorResult::ERROR_UNKNOWN;
    }

    auto CoinbaseConnector::replace_at_venue(const VenueOrderEntry &entry, const Order &replacement,
                                             std::string &new_venue_order_id) -> ConnectorResult {
        if (replacement.type != OrderType::LIMIT) {
            return ConnectorResult::ERROR_INVALID_ORDER;
        }
        const std::string payload_body = std::string(R"({"order_id":")") + entry.venue_order_id +
                                         R"(","size":")" + decimal_string(replacement.quantity) +
                                         R"(","price":")" + decimal_string(replacement.price) +
                                         R"("})";
        const auto resp = http::post(api_url() + "/api/v3/brokerage/orders/edit", payload_body,
                                     auth_headers("POST", "/api/v3/brokerage/orders/edit", payload_body, ""));
        if (!resp.ok()) {
            return classify_http_error(resp.status);
        }
        return parse_order_id(resp.body, new_venue_order_id)
                   ? ConnectorResult::OK
                   : ConnectorResult::ERROR_UNKNOWN;
    }

    auto CoinbaseConnector::query_at_venue(const VenueOrderEntry &entry,
                                           FillUpdate &status) -> ConnectorResult {
        const auto resp = http::get(
            api_url() + "/api/v3/brokerage/orders/historical/" + std::string(entry.venue_order_id),
            auth_headers(
                "GET", std::string("/api/v3/brokerage/orders/historical/") + entry.venue_order_id,
                "", ""));
        if (!resp.ok()) {
            return classify_http_error(resp.status);
        }
        return parse_query_status(resp.body, status)
                   ? ConnectorResult::OK
                   : ConnectorResult::ERROR_UNKNOWN;
    }

    auto CoinbaseConnector::cancel_all_at_venue(const char *symbol) -> ConnectorResult {
        (void) symbol;
        return ConnectorResult::ERROR_INVALID_ORDER;
    }
}

namespace trading {
    auto CoinbaseConnector::fetch_reconciliation_snapshot(ReconciliationSnapshot &snapshot)
        -> ConnectorResult {
        snapshot.clear();

        const auto open_orders =
                http::get(api_url() + "/api/v3/brokerage/orders/historical/batch",
                          auth_headers("GET", "/api/v3/brokerage/orders/historical/batch", "", ""));
        if (!open_orders.ok()) {
            return classify_http_error(open_orders.status);
        }

        ConnectorResult result = append_open_orders(open_orders.body, snapshot);
        if (result != ConnectorResult::OK) {
            return result;
        }

        const auto account_resp = http::get(api_url() + "/api/v3/brokerage/accounts",
                                            auth_headers("GET", "/api/v3/brokerage/accounts", "", ""));
        if (!account_resp.ok()) {
            return classify_http_error(account_resp.status);
        }
        result = append_balances(account_resp.body, snapshot);
        if (result != ConnectorResult::OK) {
            return result;
        }

        const auto pos_resp = http::get(api_url() + "/api/v3/brokerage/cfm/positions",
                                        auth_headers("GET", "/api/v3/brokerage/cfm/positions", "", ""));
        if (pos_resp.status != 404 && pos_resp.status != 405 && pos_resp.status != 501) {
            if (!pos_resp.ok()) {
                return classify_http_error(pos_resp.status);
            }
            result = append_positions(pos_resp.body, snapshot);
            if (result != ConnectorResult::OK) {
                return result;
            }
        }

        const auto fills_resp =
                http::get(api_url() + "/api/v3/brokerage/orders/historical/fills",
                          auth_headers("GET", "/api/v3/brokerage/orders/historical/fills", "", ""));
        if (fills_resp.status == 404 || fills_resp.status == 405 || fills_resp.status == 501) {
            return ConnectorResult::OK;
        }
        if (!fills_resp.ok()) {
            return classify_http_error(fills_resp.status);
        }
        return append_fills(fills_resp.body, snapshot);
    }
}
