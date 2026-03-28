#include "binance_futures_connector.hpp"

#include <nlohmann/json.hpp>

#include "../../common/symbol_mapper.hpp"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <string_view>
#include <unordered_map>
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

        double normalize_down(double value, double step) {
            if (step <= 0.0)
                return value;
            const long double ratio = static_cast<long double>(value) / static_cast<long double>(step);
            const long double clipped = std::floor(ratio + 1e-12L);
            return static_cast<double>(clipped * static_cast<long double>(step));
        }

        double abs_diff(double a, double b) {
            return std::fabs(a - b);
        }

        bool within_step_tolerance(double original, double normalized, double step) {
            const double epsilon = std::max(step * 1e-8, 1e-12);
            return abs_diff(original, normalized) <= epsilon;
        }

        bool parse_double_field(const nlohmann::json &json, const char *key, double &out) {
            const auto it = json.find(key);
            if (it == json.end())
                return false;
            if (it->is_number()) {
                out = it->get<double>();
                return true;
            }
            if (it->is_string()) {
                const std::string s = it->get<std::string>();
                char *end = nullptr;
                const double v = std::strtod(s.c_str(), &end);
                if (end != s.c_str()) {
                    out = v;
                    return true;
                }
            }
            return false;
        }

        bool parse_symbol_filters(const std::string &body, BinanceFuturesSymbolFilters &out) {
            const auto json = nlohmann::json::parse(body, nullptr, false);
            if (json.is_discarded())
                return false;
            const auto symbols_it = json.find("symbols");
            if (symbols_it == json.end() || !symbols_it->is_array() || symbols_it->empty())
                return false;

            const auto &symbol = (*symbols_it)[0];
            parse_double_field(symbol, "triggerProtect", out.trigger_protect);
            const auto filters_it = symbol.find("filters");
            if (filters_it == symbol.end() || !filters_it->is_array())
                return false;

            for (const auto &filter: *filters_it) {
                const std::string filter_type = filter.value("filterType", std::string());
                if (filter_type == "PRICE_FILTER") {
                    parse_double_field(filter, "minPrice", out.min_price);
                    parse_double_field(filter, "maxPrice", out.max_price);
                    parse_double_field(filter, "tickSize", out.tick_size);
                } else if (filter_type == "LOT_SIZE") {
                    parse_double_field(filter, "minQty", out.min_qty);
                    parse_double_field(filter, "maxQty", out.max_qty);
                    parse_double_field(filter, "stepSize", out.step_size);
                } else if (filter_type == "MARKET_LOT_SIZE") {
                    parse_double_field(filter, "minQty", out.min_market_qty);
                    parse_double_field(filter, "maxQty", out.max_market_qty);
                    parse_double_field(filter, "stepSize", out.market_step_size);
                } else if (filter_type == "MIN_NOTIONAL" || filter_type == "NOTIONAL") {
                    parse_double_field(filter, "notional", out.min_notional);
                    if (out.min_notional <= 0.0)
                        parse_double_field(filter, "minNotional", out.min_notional);
                }
            }

            out.valid = out.tick_size > 0.0 && out.step_size > 0.0;
            return out.valid;
        }

        ConnectorResult normalize_and_validate_order(const BinanceFuturesSymbolFilters &filters, Order &order) {
            if (order.type == OrderType::LIMIT) {
                const double normalized_price = normalize_down(order.price, filters.tick_size);
                if (normalized_price <= 0.0 || !within_step_tolerance(order.price, normalized_price, filters.tick_size))
                    return ConnectorResult::ERROR_FUTURES_PRICE_FILTER_VIOLATION;
                order.price = normalized_price;
                if ((filters.min_price > 0.0 && order.price + 1e-12 < filters.min_price) ||
                    (filters.max_price > 0.0 && order.price - 1e-12 > filters.max_price)) {
                    return ConnectorResult::ERROR_FUTURES_PRICE_FILTER_VIOLATION;
                }
            }

            if (order.type == OrderType::STOP_LIMIT) {
                const double normalized_stop = normalize_down(order.stop_price, filters.tick_size);
                if (normalized_stop <= 0.0 ||
                    !within_step_tolerance(order.stop_price, normalized_stop, filters.tick_size)) {
                    return ConnectorResult::ERROR_FUTURES_TRIGGER_CONSTRAINT_VIOLATION;
                }
                order.stop_price = normalized_stop;
                if ((filters.min_price > 0.0 && order.stop_price + 1e-12 < filters.min_price) ||
                    (filters.max_price > 0.0 && order.stop_price - 1e-12 > filters.max_price)) {
                    return ConnectorResult::ERROR_FUTURES_TRIGGER_CONSTRAINT_VIOLATION;
                }
                if (order.price > 0.0 && filters.trigger_protect > 0.0) {
                    const double distance_ratio = std::fabs(order.stop_price - order.price) / order.price;
                    if (distance_ratio - filters.trigger_protect > 1e-12)
                        return ConnectorResult::ERROR_FUTURES_TRIGGER_CONSTRAINT_VIOLATION;
                }
            }

            if (!order.close_position) {
                const bool is_market = order.type == OrderType::MARKET;
                const double min_qty = is_market && filters.min_market_qty > 0.0
                                           ? filters.min_market_qty
                                           : filters.min_qty;
                const double max_qty = is_market && filters.max_market_qty > 0.0
                                           ? filters.max_market_qty
                                           : filters.max_qty;
                const double step_size = is_market && filters.market_step_size > 0.0
                                             ? filters.market_step_size
                                             : filters.step_size;
                const double normalized_qty = normalize_down(order.quantity, step_size);
                if (normalized_qty <= 0.0 || !within_step_tolerance(order.quantity, normalized_qty, step_size))
                    return ConnectorResult::ERROR_FUTURES_QTY_FILTER_VIOLATION;
                order.quantity = normalized_qty;
                if ((min_qty > 0.0 && order.quantity + 1e-12 < min_qty) ||
                    (max_qty > 0.0 && order.quantity - 1e-12 > max_qty)) {
                    return ConnectorResult::ERROR_FUTURES_QTY_FILTER_VIOLATION;
                }

                if (filters.min_notional > 0.0) {
                    double reference_price = 0.0;
                    if (order.type == OrderType::LIMIT)
                        reference_price = order.price;
                    if (order.type == OrderType::STOP_LIMIT)
                        reference_price = order.stop_price;
                    if (reference_price > 0.0) {
                        const double notional = reference_price * order.quantity;
                        if (notional + 1e-12 < filters.min_notional)
                            return ConnectorResult::ERROR_FUTURES_MIN_NOTIONAL_VIOLATION;
                    }
                }
            }
            return ConnectorResult::OK;
        }
    }

    auto BinanceFuturesConnector::connect() -> ConnectorResult {
        const ConnectorResult result = LiveConnectorBase::connect();
        if (result != ConnectorResult::OK)
            return result;
        symbol_filters_.clear();
        return ConnectorResult::OK;
    }

    auto BinanceFuturesConnector::get_symbol_filters(const std::string &symbol,
                                                     BinanceFuturesSymbolFilters &filters)
        -> ConnectorResult {
        const auto cache_it = symbol_filters_.find(symbol);
        if (cache_it != symbol_filters_.end()) {
            filters = cache_it->second;
            return ConnectorResult::OK;
        }

        const std::string url = api_url() + "/fapi/v1/exchangeInfo?symbol=" + symbol;
        const auto response = http::get(url, {});
        if (!response.ok())
            return classify_response(response);

        BinanceFuturesSymbolFilters parsed;
        if (!parse_symbol_filters(response.body, parsed))
            return ConnectorResult::ERROR_FUTURES_FILTERS_UNAVAILABLE;
        symbol_filters_.emplace(symbol, parsed);
        filters = parsed;
        return ConnectorResult::OK;
    }

    auto BinanceFuturesConnector::submit_to_venue(const Order &order,
                                                  const std::string &idempotency_key,
                                                  std::string &venue_order_id) -> ConnectorResult {
        const std::string mapped_symbol = map_binance_futures_symbol(order.symbol);
        BinanceFuturesSymbolFilters filters;
        ConnectorResult filters_result = get_symbol_filters(mapped_symbol, filters);
        if (filters_result != ConnectorResult::OK)
            return filters_result;

        Order normalized_order = order;
        const ConnectorResult pretrade_result = normalize_and_validate_order(filters, normalized_order);
        if (pretrade_result != ConnectorResult::OK)
            return pretrade_result;

        std::vector<Param> params;
        ConnectorResult validation_result = ConnectorResult::OK;
        if (!build_order_params(normalized_order,
                                build_client_order_id(idempotency_key, normalized_order.client_order_id),
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

        const std::string mapped_symbol = map_binance_futures_symbol(replacement.symbol);
        BinanceFuturesSymbolFilters filters;
        ConnectorResult filters_result = get_symbol_filters(mapped_symbol, filters);
        if (filters_result != ConnectorResult::OK)
            return filters_result;

        Order normalized_replacement = replacement;
        const ConnectorResult pretrade_result = normalize_and_validate_order(filters, normalized_replacement);
        if (pretrade_result != ConnectorResult::OK)
            return pretrade_result;

        std::vector<Param> params = {
            {"symbol", map_binance_futures_symbol(entry.symbol)},
            {"orderId", entry.venue_order_id}
        };
        ConnectorResult validation_result = ConnectorResult::OK;
        if (!build_order_params(normalized_replacement,
                                build_client_order_id(make_idempotency_key(normalized_replacement.client_order_id),
                                                      normalized_replacement.client_order_id),
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
