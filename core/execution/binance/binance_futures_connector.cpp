#include "binance_futures_connector.hpp"

#include <nlohmann/json.hpp>

#include "../../common/symbol_mapper.hpp"

#include <cstdio>
#include <cstdlib>
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

        std::string map_binance_futures_symbol(const char *symbol) {
            return SymbolMapper::map_for_binance_usdm_futures(
                symbol == nullptr ? std::string() : std::string(symbol));
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

        std::string encode_params(const std::vector<Param> &params) {
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

        std::string side_string(Side side) {
            return side == Side::BID ? "BUY" : "SELL";
        }

        std::string position_side_string(FuturesPositionSide side) {
            switch (side) {
                case FuturesPositionSide::LONG:
                    return "LONG";
                case FuturesPositionSide::SHORT:
                    return "SHORT";
                case FuturesPositionSide::UNSPECIFIED:
                    return "BOTH";
            }
            return "BOTH";
        }

        std::string working_type_string(FuturesWorkingType type) {
            switch (type) {
                case FuturesWorkingType::CONTRACT_PRICE:
                    return "CONTRACT_PRICE";
                case FuturesWorkingType::MARK_PRICE:
                    return "MARK_PRICE";
            }
            return "CONTRACT_PRICE";
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

        ConnectorResult validate_order_request(const Order &order) {
            if (!has_non_empty_symbol(order.symbol))
                return ConnectorResult::ERROR_INVALID_ORDER;
            if (order.close_position) {
                if (order.type != OrderType::STOP_LIMIT || order.quantity > 0.0)
                    return ConnectorResult::ERROR_FUTURES_CLOSE_POSITION_CONFLICT;
            } else {
                if (order.quantity <= 0.0)
                    return ConnectorResult::ERROR_INVALID_ORDER;
            }
            if (order.type == OrderType::LIMIT && order.price <= 0.0)
                return ConnectorResult::ERROR_INVALID_ORDER;
            if (order.type == OrderType::STOP_LIMIT && order.stop_price <= 0.0)
                return ConnectorResult::ERROR_INVALID_ORDER;
            if (order.futures_position_mode == FuturesPositionMode::HEDGE) {
                if (order.futures_position_side != FuturesPositionSide::LONG &&
                    order.futures_position_side != FuturesPositionSide::SHORT) {
                    return ConnectorResult::ERROR_FUTURES_POSITION_SIDE_REQUIRED;
                }
            } else {
                if (order.futures_position_side == FuturesPositionSide::LONG ||
                    order.futures_position_side == FuturesPositionSide::SHORT) {
                    return ConnectorResult::ERROR_FUTURES_POSITION_SIDE_INVALID;
                }
            }
            return ConnectorResult::OK;
        }

        bool build_order_params(const Order &order, const std::string &client_order_id,
                                std::vector<Param> &params, ConnectorResult &validation_result) {
            validation_result = validate_order_request(order);
            if (validation_result != ConnectorResult::OK)
                return false;

            params.emplace_back("symbol", map_binance_futures_symbol(order.symbol));
            params.emplace_back("side", side_string(order.side));
            params.emplace_back("newClientOrderId", client_order_id);
            params.emplace_back("positionSide", position_side_string(
                                    order.futures_position_mode == FuturesPositionMode::HEDGE
                                        ? order.futures_position_side
                                        : FuturesPositionSide::UNSPECIFIED));
            if (order.reduce_only)
                params.emplace_back("reduceOnly", "true");

            if (order.type == OrderType::MARKET) {
                params.emplace_back("type", "MARKET");
                if (!order.close_position)
                    params.emplace_back("quantity", format_decimal(order.quantity));
                return true;
            }

            if (order.type == OrderType::STOP_LIMIT) {
                params.emplace_back("type", "STOP_MARKET");
                params.emplace_back("workingType", working_type_string(order.futures_working_type));
                params.emplace_back("stopPrice", format_decimal(order.stop_price));
                if (order.close_position) {
                    params.emplace_back("closePosition", "true");
                } else {
                    params.emplace_back("quantity", format_decimal(order.quantity));
                }
                return true;
            }

            params.emplace_back("type", "LIMIT");
            params.emplace_back("timeInForce", tif_string(order.tif));
            params.emplace_back("quantity", format_decimal(order.quantity));
            params.emplace_back("price", format_decimal(order.price));
            return true;
        }

        std::string build_client_order_id(const std::string &idempotency_key, uint64_t client_order_id) {
            if (!idempotency_key.empty())
                return idempotency_key;
            return std::string("TRT-") + std::to_string(client_order_id);
        }

        ConnectorResult classify_response(const http::HttpResponse &response) {
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

        bool parse_order_id(const std::string &body, std::string &venue_order_id) {
            const auto json = nlohmann::json::parse(body, nullptr, false);
            if (json.is_discarded())
                return false;

            const auto it = json.find("orderId");
            if (it == json.end())
                return false;
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
            return false;
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

        OrderState parse_order_status(const std::string &raw) {
            if (raw == "NEW" || raw == "PENDING_NEW")
                return OrderState::OPEN;
            if (raw == "PARTIALLY_FILLED")
                return OrderState::PARTIALLY_FILLED;
            if (raw == "FILLED")
                return OrderState::FILLED;
            if (raw == "CANCELED" || raw == "PENDING_CANCEL")
                return OrderState::CANCELED;
            if (raw == "REJECTED" || raw == "EXPIRED")
                return OrderState::REJECTED;
            return OrderState::PENDING;
        }

        bool parse_query_status(const std::string &body, FillUpdate &status) {
            const auto json = nlohmann::json::parse(body, nullptr, false);
            if (json.is_discarded())
                return false;

            status.cumulative_filled_qty = json_number(json, "executedQty");
            status.fill_qty = status.cumulative_filled_qty;
            status.avg_fill_price = json_number(json, "avgPrice");
            status.fill_price = status.avg_fill_price;
            status.new_state = parse_order_status(json.value("status", std::string()));
            status.local_ts_ns = http::now_ns();
            return true;
        }

        bool parse_cancel_result(const std::string &body) {
            const auto json = nlohmann::json::parse(body, nullptr, false);
            if (json.is_discarded())
                return false;
            if (json.find("orderId") != json.end())
                return true;
            return json.value("status", std::string()) == "CANCELED";
        }
    }

    auto BinanceFuturesConnector::submit_to_venue(const Order &order,
                                                  const std::string &idempotency_key,
                                                  std::string &venue_order_id) -> ConnectorResult {
        std::vector<Param> params;
        ConnectorResult validation_result = ConnectorResult::OK;
        if (!build_order_params(order, build_client_order_id(idempotency_key, order.client_order_id),
                                params, validation_result)) {
            return validation_result;
        }
        params.emplace_back("timestamp", std::to_string(http::now_ms()));
        params.emplace_back("recvWindow", std::to_string(recv_window_ms_));

        const std::string payload = encode_params(params);
        const std::string url = api_url() + "/fapi/v1/order?" + payload + "&signature=" +
                                hmac_sha256_hex_for_payload(payload);
        const auto response = http::post(url, "", binance_api_headers());
        if (!response.ok())
            return classify_response(response);
        if (!parse_order_id(response.body, venue_order_id))
            return ConnectorResult::ERROR_UNKNOWN;
        return ConnectorResult::OK;
    }

    auto BinanceFuturesConnector::cancel_at_venue(const VenueOrderEntry &entry) -> ConnectorResult {
        std::vector<Param> params = {
            {"symbol", map_binance_futures_symbol(entry.symbol)},
            {"orderId", entry.venue_order_id},
            {"timestamp", std::to_string(http::now_ms())},
            {"recvWindow", std::to_string(recv_window_ms_)}
        };
        const std::string payload = encode_params(params);
        const std::string url = api_url() + "/fapi/v1/order?" + payload + "&signature=" +
                                hmac_sha256_hex_for_payload(payload);
        const auto response = http::del(url, binance_api_headers());
        if (!response.ok())
            return classify_response(response);
        return parse_cancel_result(response.body) ? ConnectorResult::OK : ConnectorResult::ERROR_UNKNOWN;
    }

    auto BinanceFuturesConnector::replace_at_venue(const VenueOrderEntry &entry, const Order &replacement,
                                                   std::string &new_venue_order_id) -> ConnectorResult {
        if (std::string_view(entry.symbol) != std::string_view(replacement.symbol))
            return ConnectorResult::ERROR_INVALID_ORDER;

        std::vector<Param> params = {
            {"symbol", SymbolMapper::map_for_exchange(Exchange::BINANCE, entry.symbol)},
            {"orderId", entry.venue_order_id}
        };
        ConnectorResult validation_result = ConnectorResult::OK;
        if (!build_order_params(replacement,
                                build_client_order_id(make_idempotency_key(replacement.client_order_id),
                                                      replacement.client_order_id),
                                params, validation_result)) {
            return validation_result;
        }
        params.emplace_back("timestamp", std::to_string(http::now_ms()));
        params.emplace_back("recvWindow", std::to_string(recv_window_ms_));

        const std::string payload = encode_params(params);
        const std::string url = api_url() + "/fapi/v1/order?" + payload + "&signature=" +
                                hmac_sha256_hex_for_payload(payload);
        const auto response = http::put(url, "", binance_api_headers());
        if (!response.ok())
            return classify_response(response);
        if (!parse_order_id(response.body, new_venue_order_id))
            return ConnectorResult::ERROR_UNKNOWN;
        return ConnectorResult::OK;
    }

    auto BinanceFuturesConnector::query_at_venue(const VenueOrderEntry &entry,
                                                 FillUpdate &status) -> ConnectorResult {
        std::vector<Param> params = {
            {"symbol", map_binance_futures_symbol(entry.symbol)},
            {"orderId", entry.venue_order_id},
            {"timestamp", std::to_string(http::now_ms())},
            {"recvWindow", std::to_string(recv_window_ms_)}
        };
        const std::string payload = encode_params(params);
        const std::string url = api_url() + "/fapi/v1/order?" + payload + "&signature=" +
                                hmac_sha256_hex_for_payload(payload);
        const auto response = http::get(url, binance_api_headers());
        if (!response.ok())
            return classify_response(response);
        return parse_query_status(response.body, status)
                   ? ConnectorResult::OK
                   : ConnectorResult::ERROR_UNKNOWN;
    }

    auto BinanceFuturesConnector::cancel_all_at_venue(const char *symbol) -> ConnectorResult {
        if (!has_non_empty_symbol(symbol))
            return ConnectorResult::ERROR_INVALID_ORDER;

        std::vector<Param> params = {
            {"symbol", map_binance_futures_symbol(symbol)},
            {"timestamp", std::to_string(http::now_ms())},
            {"recvWindow", std::to_string(recv_window_ms_)}
        };
        const std::string payload = encode_params(params);
        const std::string url = api_url() + "/fapi/v1/allOpenOrders?" + payload + "&signature=" +
                                hmac_sha256_hex_for_payload(payload);
        const auto response = http::del(url, binance_api_headers());
        if (!response.ok())
            return classify_response(response);
        const auto json = nlohmann::json::parse(response.body, nullptr, false);
        if (json.is_discarded())
            return ConnectorResult::ERROR_UNKNOWN;
        return ConnectorResult::OK;
    }

    auto BinanceFuturesConnector::fetch_reconciliation_snapshot(ReconciliationSnapshot &snapshot)
        -> ConnectorResult {
        snapshot.clear();
        return ConnectorResult::ERROR_UNKNOWN;
    }
}
