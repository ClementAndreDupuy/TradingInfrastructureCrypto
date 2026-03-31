#include "binance_connector.hpp"

#include <nlohmann/json.hpp>

#include "../../common/symbol_mapper.hpp"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace trading {
    namespace {
        using Param = std::pair<std::string, std::string>;

        std::string format_decimal(double value) {
            char buffer[64] = {};
            std::snprintf(buffer, sizeof(buffer), "%.15g", value);
            return std::string(buffer);
        }

        bool has_non_empty_symbol(const char *symbol) {
            return symbol != nullptr && symbol[0] != '\0';
        }

        std::string json_string(const nlohmann::json &json, const char *key) {
            const auto it = json.find(key);
            if (it == json.end())
                return std::string();
            if (it->is_string())
                return it->get<std::string>();
            if (it->is_number_unsigned())
                return std::to_string(it->get<uint64_t>());
            if (it->is_number_integer())
                return std::to_string(it->get<int64_t>());
            if (it->is_number_float())
                return format_decimal(it->get<double>());
            return std::string();
        }

        std::string encode_component(const std::string &value) {
            static constexpr char hex[] = "0123456789ABCDEF";
            std::string out;
            out.reserve(value.size() * 3);
            for (unsigned char c: value) {
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.' || c == '~') {
                    out.push_back(static_cast<char>(c));
                } else {
                    out.push_back('%');
                    out.push_back(hex[(c >> 4) & 0x0F]);
                    out.push_back(hex[c & 0x0F]);
                }
            }
            return out;
        }

        bool validate_order_request(const Order &order) {
            if (!has_non_empty_symbol(order.symbol))
                return false;
            if (order.quantity <= 0.0)
                return false;
            if (order.type == OrderType::STOP_LIMIT)
                return false;
            if (order.type != OrderType::MARKET && order.price <= 0.0)
                return false;
            return true;
        }

        ConnectorResult classify_order_response(const http::HttpResponse &response) {
            const auto json = nlohmann::json::parse(response.body, nullptr, false);
            if (!json.is_discarded()) {
                const std::string message = json_string(json, "msg");
                if (message.find("insufficient") != std::string::npos ||
                    message.find("Insufficient") != std::string::npos) {
                    return ConnectorResult::ERROR_INSUFFICIENT_FUNDS;
                }
            }
            if (response.status == 401 || response.status == 403)
                return ConnectorResult::AUTH_FAILED;
            if (response.status == 429)
                return ConnectorResult::ERROR_RATE_LIMIT;
            if (response.status >= 500)
                return ConnectorResult::ERROR_REST_FAILURE;
            if (response.status >= 400)
                return ConnectorResult::ERROR_INVALID_ORDER;
            return ConnectorResult::ERROR_UNKNOWN;
        }

        std::string side_string(Side side) {
            return side == Side::BID ? "BUY" : "SELL";
        }

        std::string tif_string(TimeInForce tif) {
            switch (tif) {
                case TimeInForce::GTC:
                    return "GTC";
                case TimeInForce::IOC:
                    return "IOC";
                case TimeInForce::FOK:
                    return "FOK";
                case TimeInForce::GTX:
                    return "GTX";
            }
            return "GTC";
        }

        std::string percent_encode_params(const std::vector<Param> &params) {
            std::string encoded;
            bool first = true;
            for (const auto &[key, value]: params) {
                if (!first)
                    encoded.push_back('&');
                first = false;
                encoded += encode_component(key);
                encoded.push_back('=');
                encoded += encode_component(value);
            }
            return encoded;
        }

        std::string build_client_order_id(const std::string &idempotency_key, uint64_t client_order_id) {
            if (!idempotency_key.empty())
                return idempotency_key;
            return std::string("TRT-") + std::to_string(client_order_id);
        }

        bool build_order_params(const Order &order, const std::string &client_order_id,
                                std::vector<Param> &params) {
            if (!validate_order_request(order))
                return false;

            const std::string symbol = SymbolMapper::map_for_exchange(Exchange::BINANCE, order.symbol);
            params.emplace_back("symbol", symbol);
            params.emplace_back("side", side_string(order.side));
            params.emplace_back("newClientOrderId", client_order_id);

            if (order.type == OrderType::MARKET) {
                params.emplace_back("type", "MARKET");
                params.emplace_back("quantity", format_decimal(order.quantity));
                return true;
            }

            if (order.tif == TimeInForce::GTX) {
                params.emplace_back("type", "LIMIT_MAKER");
                params.emplace_back("quantity", format_decimal(order.quantity));
                params.emplace_back("price", format_decimal(order.price));
                return true;
            }

            params.emplace_back("type", "LIMIT");
            params.emplace_back("timeInForce", tif_string(order.tif));
            params.emplace_back("quantity", format_decimal(order.quantity));
            params.emplace_back("price", format_decimal(order.price));
            return true;
        }

        bool parse_order_id_value(const nlohmann::json &json, std::string &venue_order_id) {
            if (!json.is_object())
                return false;

            auto it = json.find("orderId");
            if (it != json.end()) {
                if (it->is_number_unsigned()) {
                    venue_order_id = std::to_string(it->get<uint64_t>());
                    return true;
                }
                if (it->is_number_integer()) {
                    venue_order_id = std::to_string(it->get<int64_t>());
                    return true;
                }
                if (it->is_string()) {
                    venue_order_id = it->get<std::string>();
                    return true;
                }
            }

            const auto new_order = json.find("newOrderResponse");
            if (new_order != json.end())
                return parse_order_id_value(*new_order, venue_order_id);
            const auto amended_order = json.find("amendedOrder");
            if (amended_order != json.end())
                return parse_order_id_value(*amended_order, venue_order_id);
            return false;
        }

        bool parse_order_id(const std::string &body, std::string &venue_order_id) {
            const auto json = nlohmann::json::parse(body, nullptr, false);
            if (json.is_discarded())
                return false;
            return parse_order_id_value(json, venue_order_id);
        }

        OrderState parse_order_status(const std::string &raw) {
            if (raw == "NEW" || raw == "PENDING_NEW")
                return OrderState::OPEN;
            if (raw == "PARTIALLY_FILLED")
                return OrderState::PARTIALLY_FILLED;
            if (raw == "FILLED")
                return OrderState::FILLED;
            if (raw == "CANCELED" || raw == "PENDING_CANCEL")
                return OrderState::CANCELED;
            if (raw == "REJECTED" || raw == "EXPIRED" || raw == "EXPIRED_IN_MATCH")
                return OrderState::REJECTED;
            return OrderState::PENDING;
        }

        double json_number(const nlohmann::json &json, const char *key) {
            const auto it = json.find(key);
            if (it == json.end())
                return 0.0;
            if (it->is_number())
                return it->get<double>();
            if (it->is_string()) {
                const std::string value = it->get<std::string>();
                char *end = nullptr;
                const double parsed = std::strtod(value.c_str(), &end);
                return end != value.c_str() ? parsed : 0.0;
            }
            return 0.0;
        }

        bool parse_query_status(const std::string &body, FillUpdate &status) {
            const auto json = nlohmann::json::parse(body, nullptr, false);
            if (json.is_discarded())
                return false;

            status.cumulative_filled_qty = json_number(json, "executedQty");
            status.fill_qty = status.cumulative_filled_qty;
            status.avg_fill_price = json_number(json, "avgPrice");
            if (status.avg_fill_price <= 0.0 && status.cumulative_filled_qty > 0.0) {
                const double cumulative_quote_qty = json_number(json, "cummulativeQuoteQty");
                status.avg_fill_price = cumulative_quote_qty / status.cumulative_filled_qty;
            }
            status.fill_price = status.avg_fill_price;
            status.new_state = parse_order_status(json.value("status", std::string()));
            status.local_ts_ns = http::now_ns();
            return true;
        }

        bool parse_cancel_result(const std::string &body) {
            const auto json = nlohmann::json::parse(body, nullptr, false);
            if (json.is_discarded())
                return false;
            if (json.find("cancelResult") != json.end())
                return json.value("cancelResult", std::string()) == "SUCCESS";
            if (json.find("orderId") != json.end())
                return true;
            return json.value("status", std::string()) == "CANCELED";
        }

        uint64_t parse_client_order_id(const std::string &client_order_id) {
            uint64_t value = 0;
            bool found_digit = false;
            for (char c: client_order_id) {
                if (c >= '0' && c <= '9') {
                    found_digit = true;
                    value = value * 10 + static_cast<uint64_t>(c - '0');
                }
            }
            return found_digit ? value : 0;
        }
    }

    auto BinanceConnector::submit_to_venue(const Order &order, const std::string &idempotency_key,
                                           std::string &venue_order_id) -> ConnectorResult {
        if (!validate_order_request(order))
            return ConnectorResult::ERROR_INVALID_ORDER;

        std::vector<Param> params;
        if (!build_order_params(order, build_client_order_id(idempotency_key, order.client_order_id),
                                params)) {
            return ConnectorResult::ERROR_INVALID_ORDER;
        }
        params.emplace_back("newOrderRespType", "FULL");
        params.emplace_back("timestamp", std::to_string(http::now_ms()));

        const std::string payload = percent_encode_params(params);
        const std::string signed_query = payload + "&signature=" + hmac_sha256_hex_for_payload(payload);
        const auto response =
                http::post(api_url() + "/api/v3/order?" + signed_query, "", binance_api_headers());
        if (!response.ok())
            return classify_order_response(response);
        if (!parse_order_id(response.body, venue_order_id))
            return ConnectorResult::ERROR_UNKNOWN;
        return ConnectorResult::OK;
    }

    auto BinanceConnector::cancel_at_venue(const VenueOrderEntry &entry) -> ConnectorResult {
        std::vector<Param> params = {
            {"symbol", SymbolMapper::map_for_exchange(Exchange::BINANCE, entry.symbol)},
            {"orderId", entry.venue_order_id},
            {"timestamp", std::to_string(http::now_ms())}
        };
        const std::string payload = percent_encode_params(params);
        const auto response = http::del(api_url() + "/api/v3/order?" + payload + "&signature=" +
                                        hmac_sha256_hex_for_payload(payload),
                                        binance_api_headers());
        if (!response.ok())
            return classify_order_response(response);
        return parse_cancel_result(response.body) ? ConnectorResult::OK : ConnectorResult::ERROR_UNKNOWN;
    }

    auto BinanceConnector::replace_at_venue(const VenueOrderEntry &entry, const Order &replacement,
                                            std::string &new_venue_order_id) -> ConnectorResult {
        if (!validate_order_request(replacement))
            return ConnectorResult::ERROR_INVALID_ORDER;
        if (std::string_view(entry.symbol) != std::string_view(replacement.symbol))
            return ConnectorResult::ERROR_INVALID_ORDER;

        std::vector<Param> params = {
            {"symbol", SymbolMapper::map_for_exchange(Exchange::BINANCE, entry.symbol)},
            {"cancelOrderId", entry.venue_order_id},
            {"cancelReplaceMode", "STOP_ON_FAILURE"}
        };
        if (!build_order_params(replacement,
                                build_client_order_id(make_idempotency_key(replacement.client_order_id),
                                                      replacement.client_order_id),
                                params)) {
            return ConnectorResult::ERROR_INVALID_ORDER;
        }
        params.emplace_back("newOrderRespType", "FULL");
        params.emplace_back("timestamp", std::to_string(http::now_ms()));

        const std::string payload = percent_encode_params(params);
        const auto response = http::post(api_url() + "/api/v3/order/cancelReplace?" + payload +
                                         "&signature=" + hmac_sha256_hex_for_payload(payload),
                                         "", binance_api_headers());
        if (!response.ok())
            return classify_order_response(response);
        if (!parse_cancel_result(response.body) &&
            response.body.find("newOrderResponse") == std::string::npos) {
            return ConnectorResult::ERROR_UNKNOWN;
        }
        if (!parse_order_id(response.body, new_venue_order_id))
            return ConnectorResult::ERROR_UNKNOWN;
        return ConnectorResult::OK;
    }

    auto BinanceConnector::query_at_venue(const VenueOrderEntry &entry,
                                          FillUpdate &status) -> ConnectorResult {
        std::vector<Param> params = {
            {"symbol", SymbolMapper::map_for_exchange(Exchange::BINANCE, entry.symbol)},
            {"orderId", entry.venue_order_id},
            {"timestamp", std::to_string(http::now_ms())}
        };
        const std::string payload = percent_encode_params(params);
        const auto response = http::get(api_url() + "/api/v3/order?" + payload + "&signature=" +
                                        hmac_sha256_hex_for_payload(payload),
                                        binance_api_headers());
        if (!response.ok())
            return classify_order_response(response);
        return parse_query_status(response.body, status)
                   ? ConnectorResult::OK
                   : ConnectorResult::ERROR_UNKNOWN;
    }

    auto BinanceConnector::cancel_all_at_venue(const char *symbol) -> ConnectorResult {
        if (!has_non_empty_symbol(symbol))
            return ConnectorResult::ERROR_INVALID_ORDER;

        std::vector<Param> params = {
            {"symbol", SymbolMapper::map_for_exchange(Exchange::BINANCE, symbol)},
            {"timestamp", std::to_string(http::now_ms())}
        };
        const std::string payload = percent_encode_params(params);
        const auto response = http::del(api_url() + "/api/v3/openOrders?" + payload + "&signature=" +
                                        hmac_sha256_hex_for_payload(payload),
                                        binance_api_headers());
        if (!response.ok())
            return classify_order_response(response);
        const auto json = nlohmann::json::parse(response.body, nullptr, false);
        return (!json.is_discarded() && json.is_array())
                   ? ConnectorResult::OK
                   : ConnectorResult::ERROR_UNKNOWN;
    }
}

namespace trading {
    auto BinanceConnector::fetch_reconciliation_snapshot(ReconciliationSnapshot &snapshot)
        -> ConnectorResult {
        snapshot.clear();

        const std::vector<Param> account_params = {{"timestamp", std::to_string(http::now_ms())}};
        const std::string account_payload = percent_encode_params(account_params);
        const auto account_response = http::get(api_url() + "/api/v3/account?" + account_payload +
                                                "&signature=" +
                                                hmac_sha256_hex_for_payload(account_payload),
                                                binance_api_headers());
        if (!account_response.ok()) {
            LOG_ERROR("[Binance] Reconciliation account fetch failed", "status",
                      account_response.status, "body",
                      account_response.body.substr(0, 256).c_str());
            return classify_order_response(account_response);
        }

        const auto account_json = nlohmann::json::parse(account_response.body, nullptr, false);
        if (account_json.is_discarded()) {
            LOG_ERROR("[Binance] Reconciliation account response JSON parse failed");
            return ConnectorResult::ERROR_UNKNOWN;
        }
        const auto &balances = account_json["balances"];
        if (!balances.is_array()) {
            LOG_ERROR("[Binance] Reconciliation account response missing balances array");
            return ConnectorResult::ERROR_UNKNOWN;
        }

        for (const auto &item: balances) {
            ReconciledBalance balance;
            copy_cstr(balance.asset, sizeof(balance.asset), item.value("asset", std::string()));
            balance.total = json_number(item, "free") + json_number(item, "locked");
            balance.available = json_number(item, "free");
            if (!snapshot.balances.push(balance))
                return ConnectorResult::ERROR_UNKNOWN;
        }

        const std::vector<Param> open_order_params = {{"timestamp", std::to_string(http::now_ms())}};
        const std::string open_order_payload = percent_encode_params(open_order_params);
        const auto open_order_response =
                http::get(api_url() + "/api/v3/openOrders?" + open_order_payload + "&signature=" +
                          hmac_sha256_hex_for_payload(open_order_payload),
                          binance_api_headers());
        if (!open_order_response.ok()) {
            LOG_ERROR("[Binance] Reconciliation open orders fetch failed", "status",
                      open_order_response.status, "body",
                      open_order_response.body.substr(0, 256).c_str());
            return classify_order_response(open_order_response);
        }

        std::set<std::string> symbols;
        venue_order_map().for_each_active([&](const VenueOrderEntry &entry) {
            if (entry.symbol[0] != '\0')
                symbols.insert(SymbolMapper::map_for_exchange(Exchange::BINANCE, entry.symbol));
        });

        const auto open_order_json = nlohmann::json::parse(open_order_response.body, nullptr, false);
        if (!open_order_json.is_array())
            return ConnectorResult::ERROR_UNKNOWN;

        for (const auto &item: open_order_json) {
            const std::string symbol = json_string(item, "symbol");
            if (!symbol.empty())
                symbols.insert(symbol);

            ReconciledOrder order;
            order.client_order_id = parse_client_order_id(json_string(item, "clientOrderId"));
            copy_cstr(order.venue_order_id, sizeof(order.venue_order_id), json_string(item, "orderId"));
            copy_cstr(order.symbol, sizeof(order.symbol), symbol);
            order.side = item.value("side", std::string("BUY")) == "SELL" ? Side::ASK : Side::BID;
            order.quantity = json_number(item, "origQty");
            order.filled_quantity = json_number(item, "executedQty");
            order.price = json_number(item, "price");
            order.state = parse_order_status(item.value("status", std::string()));
            if (!snapshot.open_orders.push(order))
                return ConnectorResult::ERROR_UNKNOWN;
        }

        for (const std::string &symbol: symbols) {
            const std::vector<Param> trades_params = {
                {"symbol", symbol},
                {"timestamp", std::to_string(http::now_ms())}
            };
            const std::string trades_payload = percent_encode_params(trades_params);
            const auto trades_response = http::get(api_url() + "/api/v3/myTrades?" + trades_payload +
                                                   "&signature=" +
                                                   hmac_sha256_hex_for_payload(trades_payload),
                                                   binance_api_headers());
            if (trades_response.status == 404 || trades_response.status == 405 ||
                trades_response.status == 501) {
                continue;
            }
            if (!trades_response.ok()) {
                LOG_ERROR("[Binance] Reconciliation trades fetch failed", "symbol", symbol.c_str(),
                          "status", trades_response.status, "body",
                          trades_response.body.substr(0, 256).c_str());
                return classify_order_response(trades_response);
            }

            const auto trades_json = nlohmann::json::parse(trades_response.body, nullptr, false);
            if (!trades_json.is_array())
                return ConnectorResult::ERROR_UNKNOWN;
            for (const auto &item: trades_json) {
                ReconciledFill fill;
                fill.exchange = Exchange::BINANCE;
                fill.client_order_id = item.value("orderId", static_cast<uint64_t>(0));
                copy_cstr(fill.venue_order_id, sizeof(fill.venue_order_id),
                          std::to_string(item.value("orderId", static_cast<uint64_t>(0))));
                copy_cstr(fill.venue_trade_id, sizeof(fill.venue_trade_id),
                          std::to_string(item.value("id", static_cast<uint64_t>(0))));
                copy_cstr(fill.symbol, sizeof(fill.symbol), item.value("symbol", std::string()));
                fill.side = item.value("isBuyer", true) ? Side::BID : Side::ASK;
                fill.quantity = json_number(item, "qty");
                fill.price = json_number(item, "price");
                fill.notional = fill.quantity * fill.price;
                fill.fee = json_number(item, "commission");
                copy_cstr(fill.fee_asset, sizeof(fill.fee_asset),
                          item.value("commissionAsset", std::string()));
                fill.exchange_ts_ns = item.value("time", static_cast<int64_t>(0)) * 1000000;
                if (!snapshot.fills.push(fill))
                    return ConnectorResult::ERROR_UNKNOWN;
            }
        }

        return ConnectorResult::OK;
    }
}
