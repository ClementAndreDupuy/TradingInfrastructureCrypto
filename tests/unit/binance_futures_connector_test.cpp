#include "core/common/rest_client.hpp"
#include "core/execution/binance/binance_futures_connector.hpp"

#include <gtest/gtest.h>
#include <openssl/hmac.h>
#include <nlohmann/json.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace trading {
namespace {

class ScopedMockTransport {
  public:
    explicit ScopedMockTransport(http::MockTransport handler) {
        http::set_mock_transport(std::move(handler));
    }
    ~ScopedMockTransport() { http::clear_mock_transport(); }
};

class TestableBinanceFuturesConnector : public BinanceFuturesConnector {
  public:
    using BinanceFuturesConnector::BinanceFuturesConnector;
    using BinanceFuturesConnector::fetch_reconciliation_snapshot;
};

bool contains(const std::string &s, const char *token) {
    return s.find(token) != std::string::npos;
}

std::string hmac_hex(const std::string &secret, const std::string &payload) {
    unsigned char digest[EVP_MAX_MD_SIZE] = {};
    unsigned int len = 0;
    HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
         reinterpret_cast<const unsigned char *>(payload.data()), payload.size(), digest, &len);
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (unsigned int i = 0; i < len; ++i) {
        out.push_back(hex[(digest[i] >> 4) & 0x0F]);
        out.push_back(hex[digest[i] & 0x0F]);
    }
    return out;
}

std::string extract_query(const std::string &url) {
    const auto q = url.find('?');
    if (q == std::string::npos)
        return std::string();
    return url.substr(q + 1);
}

std::string query_value(const std::string &query, const std::string &key) {
    const std::string token = key + "=";
    const auto start = query.find(token);
    if (start == std::string::npos)
        return std::string();
    const auto end = query.find('&', start);
    if (end == std::string::npos)
        return query.substr(start + token.size());
    return query.substr(start + token.size(), end - (start + token.size()));
}

std::string exchange_info_for(const std::string& symbol, const std::string& min_notional = "5") {
    return std::string(R"({"symbols":[{"symbol":")") + symbol +
           R"(","triggerProtect":"0.0500","filters":[{"filterType":"PRICE_FILTER","minPrice":"0.10","maxPrice":"1000000","tickSize":"0.10"},{"filterType":"LOT_SIZE","minQty":"0.001","maxQty":"1000","stepSize":"0.001"},{"filterType":"MARKET_LOT_SIZE","minQty":"0.001","maxQty":"500","stepSize":"0.001"},{"filterType":"MIN_NOTIONAL","notional":")" +
           min_notional + R"("}]}]})";
}

Order make_order(uint64_t id) {
    Order o{};
    o.client_order_id = id;
    o.exchange = Exchange::BINANCE;
    o.side = Side::BID;
    o.type = OrderType::LIMIT;
    o.tif = TimeInForce::IOC;
    o.price = 100.0;
    o.quantity = 0.1;
    std::strncpy(o.symbol, "BTCUSDT", sizeof(o.symbol) - 1);
    o.symbol[sizeof(o.symbol) - 1] = '\0';
    return o;
}

std::filesystem::path futures_fixture_path() {
    const std::filesystem::path here(__FILE__);
    return here.parent_path().parent_path() / "data" / "binance" / "futures_error_fixtures.json";
}

ConnectorResult connector_result_from_name(const std::string& name) {
    if (name == "AUTH_FAILED")
        return ConnectorResult::AUTH_FAILED;
    if (name == "ERROR_RATE_LIMIT")
        return ConnectorResult::ERROR_RATE_LIMIT;
    if (name == "ERROR_REST_FAILURE")
        return ConnectorResult::ERROR_REST_FAILURE;
    if (name == "ERROR_INVALID_ORDER")
        return ConnectorResult::ERROR_INVALID_ORDER;
    return ConnectorResult::ERROR_UNKNOWN;
}

} 

TEST(BinanceFuturesConnectorTest, RoutesOnlyToFapiEndpoints) {
    BinanceFuturesConnector c("k", "s", "https://futures.test", 7777);
    int submit_calls = 0;
    int query_calls = 0;
    int replace_calls = 0;
    int cancel_calls = 0;
    int cancel_all_calls = 0;

    ScopedMockTransport transport([&](const char *method, const std::string &url,
                                      const std::string &, const std::vector<std::string> &) {
        EXPECT_FALSE(contains(url, "/api/v3"));
        if (std::strcmp(method, "GET") == 0 && contains(url, "/fapi/v1/exchangeInfo?symbol=BTCUSDT")) {
            return http::HttpResponse{200, exchange_info_for("BTCUSDT")};
        }
        if (std::strcmp(method, "POST") == 0 && contains(url, "/fapi/v1/order?")) {
            ++submit_calls;
            EXPECT_TRUE(contains(url, "recvWindow=7777"));
            return http::HttpResponse{200, R"({"orderId":42})"};
        }
        if (std::strcmp(method, "GET") == 0 && contains(url, "/fapi/v1/order?")) {
            ++query_calls;
            EXPECT_TRUE(contains(url, "recvWindow=7777"));
            return http::HttpResponse{200, R"({"status":"NEW","executedQty":"0","avgPrice":"0"})"};
        }
        if (std::strcmp(method, "PUT") == 0 && contains(url, "/fapi/v1/order?")) {
            ++replace_calls;
            EXPECT_TRUE(contains(url, "recvWindow=7777"));
            return http::HttpResponse{200, R"({"orderId":43})"};
        }
        if (std::strcmp(method, "DELETE") == 0 && contains(url, "/fapi/v1/order?")) {
            ++cancel_calls;
            EXPECT_TRUE(contains(url, "recvWindow=7777"));
            return http::HttpResponse{200, R"({"orderId":43,"status":"CANCELED"})"};
        }
        if (std::strcmp(method, "DELETE") == 0 && contains(url, "/fapi/v1/allOpenOrders?")) {
            ++cancel_all_calls;
            EXPECT_TRUE(contains(url, "recvWindow=7777"));
            return http::HttpResponse{200, R"({"code":200,"msg":"done"})"};
        }
        return http::HttpResponse{404, ""};
    });

    ASSERT_EQ(c.connect(), ConnectorResult::OK);
    Order order = make_order(42);
    ASSERT_EQ(c.submit_order(order), ConnectorResult::OK);

    FillUpdate status{};
    ASSERT_EQ(c.query_order(42, status), ConnectorResult::OK);

    Order replacement = order;
    replacement.client_order_id = 43;
    replacement.price = 101.0;
    ASSERT_EQ(c.replace_order(42, replacement), ConnectorResult::OK);
    ASSERT_EQ(c.cancel_order(43), ConnectorResult::OK);
    ASSERT_EQ(c.cancel_all("BTCUSDT"), ConnectorResult::OK);

    EXPECT_EQ(submit_calls, 1);
    EXPECT_EQ(query_calls, 1);
    EXPECT_EQ(replace_calls, 1);
    EXPECT_EQ(cancel_calls, 1);
    EXPECT_EQ(cancel_all_calls, 1);
}

TEST(BinanceFuturesConnectorTest, SignedQueryIncludesTimestampRecvWindowAndSignature) {
    const std::string secret = "futures-secret";
    BinanceFuturesConnector c("api-key", secret, "https://futures.test", 2500);

    ScopedMockTransport transport([&](const char *method, const std::string &url,
                                      const std::string &, const std::vector<std::string> &) {
        if (std::strcmp(method, "GET") == 0 && contains(url, "/fapi/v1/exchangeInfo?symbol=BTCUSDT")) {
            return http::HttpResponse{200, exchange_info_for("BTCUSDT")};
        }
        EXPECT_EQ(std::strcmp(method, "POST"), 0);
        EXPECT_TRUE(contains(url, "/fapi/v1/order?"));

        const std::string query = extract_query(url);
        const std::string signature = query_value(query, "signature");
        EXPECT_FALSE(signature.empty());

        const std::string payload = query.substr(0, query.find("&signature="));
        EXPECT_FALSE(query_value(query, "timestamp").empty());
        EXPECT_EQ(query_value(query, "recvWindow"), "2500");
        EXPECT_EQ(signature, hmac_hex(secret, payload));

        return http::HttpResponse{200, R"({"orderId":7001})"};
    });

    ASSERT_EQ(c.connect(), ConnectorResult::OK);
    ASSERT_EQ(c.submit_order(make_order(7001)), ConnectorResult::OK);
}

TEST(BinanceFuturesConnectorTest, OneWayModeMapsToBothPositionSide) {
    BinanceFuturesConnector c("k", "s", "https://futures.test", 4000);
    ScopedMockTransport transport([&](const char *method, const std::string &url,
                                      const std::string &, const std::vector<std::string> &) {
        if (std::strcmp(method, "GET") == 0 && contains(url, "/fapi/v1/exchangeInfo?symbol=BTCUSDT")) {
            return http::HttpResponse{200, exchange_info_for("BTCUSDT")};
        }
        EXPECT_EQ(std::strcmp(method, "POST"), 0);
        EXPECT_TRUE(contains(url, "positionSide=BOTH"));
        EXPECT_TRUE(contains(url, "reduceOnly=true"));
        return http::HttpResponse{200, R"({"orderId":8001})"};
    });

    Order order = make_order(8001);
    order.futures_position_mode = FuturesPositionMode::ONE_WAY;
    order.futures_position_side = FuturesPositionSide::UNSPECIFIED;
    order.reduce_only = true;
    ASSERT_EQ(c.connect(), ConnectorResult::OK);
    ASSERT_EQ(c.submit_order(order), ConnectorResult::OK);
}

TEST(BinanceFuturesConnectorTest, HedgeModeRequiresExplicitLongShortPositionSide) {
    BinanceFuturesConnector c("k", "s", "https://futures.test", 4000);
    Order invalid = make_order(8101);
    invalid.futures_position_mode = FuturesPositionMode::HEDGE;
    invalid.futures_position_side = FuturesPositionSide::UNSPECIFIED;
    ASSERT_EQ(c.connect(), ConnectorResult::OK);
    EXPECT_EQ(c.submit_order(invalid), ConnectorResult::ERROR_FUTURES_POSITION_SIDE_REQUIRED);

    ScopedMockTransport transport([&](const char *method, const std::string &url,
                                      const std::string &, const std::vector<std::string> &) {
        if (std::strcmp(method, "GET") == 0 && contains(url, "/fapi/v1/exchangeInfo?symbol=BTCUSDT")) {
            return http::HttpResponse{200, exchange_info_for("BTCUSDT")};
        }
        EXPECT_EQ(std::strcmp(method, "POST"), 0);
        EXPECT_TRUE(contains(url, "positionSide=LONG"));
        return http::HttpResponse{200, R"({"orderId":8102})"};
    });

    Order valid = make_order(8102);
    valid.futures_position_mode = FuturesPositionMode::HEDGE;
    valid.futures_position_side = FuturesPositionSide::LONG;
    ASSERT_EQ(c.submit_order(valid), ConnectorResult::OK);
}

TEST(BinanceFuturesConnectorTest, RejectsClosePositionWithQuantityConflict) {
    BinanceFuturesConnector c("k", "s", "https://futures.test", 4000);
    ScopedMockTransport transport([](const char *, const std::string &, const std::string &,
                                     const std::vector<std::string> &) {
        ADD_FAILURE() << "unexpected HTTP call";
        return http::HttpResponse{500, ""};
    });

    Order order = make_order(8201);
    order.type = OrderType::STOP_LIMIT;
    order.stop_price = 99.5;
    order.close_position = true;
    order.quantity = 0.01;
    ASSERT_EQ(c.connect(), ConnectorResult::OK);
    EXPECT_EQ(c.submit_order(order), ConnectorResult::ERROR_FUTURES_CLOSE_POSITION_CONFLICT);
}

TEST(BinanceFuturesConnectorTest, NormalizesPerpSymbolForFuturesEndpoints) {
    BinanceFuturesConnector c("k", "s", "https://futures.test", 4000);
    ScopedMockTransport transport([&](const char *method, const std::string &url,
                                      const std::string &, const std::vector<std::string> &) {
        if (std::strcmp(method, "GET") == 0 && contains(url, "/fapi/v1/exchangeInfo?symbol=BTCUSDT")) {
            return http::HttpResponse{200, exchange_info_for("BTCUSDT")};
        }
        EXPECT_EQ(std::strcmp(method, "POST"), 0);
        EXPECT_TRUE(contains(url, "symbol=BTCUSDT"));
        EXPECT_FALSE(contains(url, "PERP"));
        return http::HttpResponse{200, R"({"orderId":8301})"};
    });

    Order order = make_order(8301);
    std::strncpy(order.symbol, "BTC-USDT-PERP", sizeof(order.symbol) - 1);
    order.symbol[sizeof(order.symbol) - 1] = '\0';
    ASSERT_EQ(c.connect(), ConnectorResult::OK);
    ASSERT_EQ(c.submit_order(order), ConnectorResult::OK);
}

TEST(BinanceFuturesConnectorTest, KeepsBtcUsdtStableForFuturesEndpoints) {
    BinanceFuturesConnector c("k", "s", "https://futures.test", 4000);
    ScopedMockTransport transport([&](const char *method, const std::string &url,
                                      const std::string &, const std::vector<std::string> &) {
        if (std::strcmp(method, "GET") == 0 && contains(url, "/fapi/v1/exchangeInfo?symbol=BTCUSDT")) {
            return http::HttpResponse{200, exchange_info_for("BTCUSDT")};
        }
        EXPECT_EQ(std::strcmp(method, "POST"), 0);
        EXPECT_TRUE(contains(url, "symbol=BTCUSDT"));
        return http::HttpResponse{200, R"({"orderId":8302})"};
    });

    Order order = make_order(8302);
    std::strncpy(order.symbol, "BTCUSDT", sizeof(order.symbol) - 1);
    order.symbol[sizeof(order.symbol) - 1] = '\0';
    ASSERT_EQ(c.connect(), ConnectorResult::OK);
    ASSERT_EQ(c.submit_order(order), ConnectorResult::OK);
}

TEST(BinanceFuturesConnectorTest, RejectsQuantityStepViolationBeforeSubmit) {
    BinanceFuturesConnector c("k", "s", "https://futures.test", 4000);
    ScopedMockTransport transport([](const char *method, const std::string &url, const std::string &,
                                     const std::vector<std::string> &) {
        if (std::strcmp(method, "GET") == 0 && contains(url, "/fapi/v1/exchangeInfo?symbol=BTCUSDT")) {
            return http::HttpResponse{200, exchange_info_for("BTCUSDT")};
        }
        ADD_FAILURE() << "unexpected HTTP call";
        return http::HttpResponse{500, ""};
    });

    Order order = make_order(8401);
    order.quantity = 0.0015;
    ASSERT_EQ(c.connect(), ConnectorResult::OK);
    EXPECT_EQ(c.submit_order(order), ConnectorResult::ERROR_FUTURES_QTY_FILTER_VIOLATION);
}

TEST(BinanceFuturesConnectorTest, RejectsMinNotionalViolationForAltPerp) {
    BinanceFuturesConnector c("k", "s", "https://futures.test", 4000);
    ScopedMockTransport transport([](const char *method, const std::string &url, const std::string &,
                                     const std::vector<std::string> &) {
        if (std::strcmp(method, "GET") == 0 && contains(url, "/fapi/v1/exchangeInfo?symbol=ETHUSDT")) {
            return http::HttpResponse{200, exchange_info_for("ETHUSDT", "100")};
        }
        ADD_FAILURE() << "unexpected HTTP call";
        return http::HttpResponse{500, ""};
    });

    Order order = make_order(8402);
    std::strncpy(order.symbol, "ETHUSDT", sizeof(order.symbol) - 1);
    order.symbol[sizeof(order.symbol) - 1] = '\0';
    order.price = 50.0;
    order.quantity = 1.0;
    ASSERT_EQ(c.connect(), ConnectorResult::OK);
    EXPECT_EQ(c.submit_order(order), ConnectorResult::ERROR_FUTURES_MIN_NOTIONAL_VIOLATION);
}

TEST(BinanceFuturesConnectorTest, RejectsStopTriggerProtectViolation) {
    BinanceFuturesConnector c("k", "s", "https://futures.test", 4000);
    ScopedMockTransport transport([](const char *method, const std::string &url, const std::string &,
                                     const std::vector<std::string> &) {
        if (std::strcmp(method, "GET") == 0 && contains(url, "/fapi/v1/exchangeInfo?symbol=BTCUSDT")) {
            return http::HttpResponse{200, exchange_info_for("BTCUSDT")};
        }
        ADD_FAILURE() << "unexpected HTTP call";
        return http::HttpResponse{500, ""};
    });

    Order order = make_order(8403);
    order.type = OrderType::STOP_LIMIT;
    order.price = 100.0;
    order.stop_price = 120.0;
    order.quantity = 0.01;
    ASSERT_EQ(c.connect(), ConnectorResult::OK);
    EXPECT_EQ(c.submit_order(order), ConnectorResult::ERROR_FUTURES_TRIGGER_CONSTRAINT_VIOLATION);
}

TEST(BinanceFuturesConnectorTest, FetchesFuturesReconciliationSnapshotWithPositionsAndLeverage) {
    TestableBinanceFuturesConnector c("k", "s", "https://futures.test", 4000);
    int position_calls = 0;
    int open_order_calls = 0;
    int balance_calls = 0;
    int trades_calls = 0;

    ScopedMockTransport transport([&](const char *method, const std::string &url,
                                      const std::string &, const std::vector<std::string> &) {
        if (std::strcmp(method, "GET") != 0)
            return http::HttpResponse{404, ""};
        if (contains(url, "/fapi/v2/balance?")) {
            ++balance_calls;
            return http::HttpResponse{
                200,
                R"([{"asset":"USDT","balance":"1100","availableBalance":"1000"}])"};
        }
        if (contains(url, "/fapi/v1/openOrders?")) {
            ++open_order_calls;
            return http::HttpResponse{
                200,
                R"([{"orderId":"123","symbol":"BTCUSDT","clientOrderId":"TRT-123-BINANCE","side":"BUY","origQty":"0.50","executedQty":"0.10","price":"100.0","status":"NEW"}])"};
        }
        if (contains(url, "/fapi/v2/positionRisk?")) {
            ++position_calls;
            return http::HttpResponse{
                200,
                R"([{"symbol":"BTCUSDT","positionAmt":"0.40","entryPrice":"99.5","positionSide":"LONG","leverage":"15"},{"symbol":"BTCUSDT","positionAmt":"-0.10","entryPrice":"101.0","positionSide":"SHORT","leverage":"15"}])"};
        }
        if (contains(url, "/fapi/v1/userTrades?symbol=BTCUSDT")) {
            ++trades_calls;
            return http::HttpResponse{
                200,
                R"([{"id":"9001","orderId":"123","clientOrderId":"TRT-123-BINANCE","symbol":"BTCUSDT","side":"BUY","qty":"0.1","price":"100.0","commission":"0.0005","commissionAsset":"USDT","time":1700000000000}])"};
        }
        return http::HttpResponse{404, ""};
    });

    ASSERT_EQ(c.connect(), ConnectorResult::OK);
    ReconciliationSnapshot snapshot;
    ASSERT_EQ(c.fetch_reconciliation_snapshot(snapshot), ConnectorResult::OK);

    ASSERT_EQ(snapshot.balances.size, 1U);
    EXPECT_STREQ(snapshot.balances.items[0].asset, "USDT");
    EXPECT_DOUBLE_EQ(snapshot.balances.items[0].total, 1100.0);
    EXPECT_DOUBLE_EQ(snapshot.balances.items[0].available, 1000.0);

    ASSERT_EQ(snapshot.open_orders.size, 1U);
    EXPECT_EQ(snapshot.open_orders.items[0].client_order_id, 123U);
    EXPECT_STREQ(snapshot.open_orders.items[0].symbol, "BTCUSDT");

    ASSERT_EQ(snapshot.positions.size, 2U);
    EXPECT_STREQ(snapshot.positions.items[0].position_side, "LONG");
    EXPECT_DOUBLE_EQ(snapshot.positions.items[0].leverage, 15.0);
    EXPECT_STREQ(snapshot.positions.items[1].position_side, "SHORT");
    EXPECT_DOUBLE_EQ(snapshot.positions.items[1].quantity, -0.10);

    ASSERT_EQ(snapshot.fills.size, 1U);
    EXPECT_STREQ(snapshot.fills.items[0].venue_trade_id, "9001");
    EXPECT_DOUBLE_EQ(snapshot.fills.items[0].quantity, 0.1);

    EXPECT_EQ(balance_calls, 1);
    EXPECT_EQ(open_order_calls, 1);
    EXPECT_EQ(position_calls, 1);
    EXPECT_EQ(trades_calls, 1);
}

TEST(BinanceFuturesConnectorTest, AppliesDeterministicErrorFixturesForSubmitClassification) {
    BinanceFuturesConnector c("k", "s", "https://futures.test", 4000);
    std::ifstream fixture_stream(futures_fixture_path());
    ASSERT_TRUE(fixture_stream.good());

    nlohmann::json fixtures;
    fixture_stream >> fixtures;
    ASSERT_TRUE(fixtures.is_array());

    ASSERT_EQ(c.connect(), ConnectorResult::OK);
    for (const auto& fixture : fixtures) {
        const int status = fixture.value("status", 0);
        const std::string body = fixture.value("body", std::string("{}"));
        const ConnectorResult expected =
            connector_result_from_name(fixture.value("expected", std::string()));
        const int expected_attempts = fixture.value("expected_attempts", 1);
        int submit_attempts = 0;
        int exchange_info_calls = 0;

        ScopedMockTransport transport([&](const char *method, const std::string &url,
                                          const std::string &, const std::vector<std::string> &) {
            if (std::strcmp(method, "GET") == 0 && contains(url, "/fapi/v1/exchangeInfo?symbol=BTCUSDT")) {
                ++exchange_info_calls;
                return http::HttpResponse{200, exchange_info_for("BTCUSDT")};
            }
            if (std::strcmp(method, "POST") == 0 && contains(url, "/fapi/v1/order?")) {
                ++submit_attempts;
                return http::HttpResponse{status, body};
            }
            return http::HttpResponse{404, ""};
        });

        Order order = make_order(9000 + fixture.value("id", 0));
        EXPECT_EQ(c.submit_order(order), expected);
        EXPECT_EQ(submit_attempts, expected_attempts);
        EXPECT_EQ(exchange_info_calls, 1);
        EXPECT_EQ(c.venue_order_map().get(order.client_order_id), nullptr);
    }
}

TEST(BinanceFuturesConnectorTest, MaintainsStateInvariantsAcrossFailurePaths) {
    BinanceFuturesConnector c("k", "s", "https://futures.test", 4000);
    ASSERT_EQ(c.connect(), ConnectorResult::OK);

    {
        ScopedMockTransport transport([&](const char *method, const std::string &url,
                                          const std::string &, const std::vector<std::string> &) {
            if (std::strcmp(method, "GET") == 0 && contains(url, "/fapi/v1/exchangeInfo?symbol=BTCUSDT"))
                return http::HttpResponse{200, exchange_info_for("BTCUSDT")};
            if (std::strcmp(method, "POST") == 0 && contains(url, "/fapi/v1/order?"))
                return http::HttpResponse{200, R"({"orderId":9101})"};
            return http::HttpResponse{404, ""};
        });
        ASSERT_EQ(c.submit_order(make_order(9101)), ConnectorResult::OK);
    }

    const VenueOrderEntry* seeded = c.venue_order_map().get(9101);
    ASSERT_NE(seeded, nullptr);

    {
        int attempts = 0;
        ScopedMockTransport transport([&](const char *method, const std::string &url,
                                          const std::string &, const std::vector<std::string> &) {
            if (std::strcmp(method, "DELETE") == 0 && contains(url, "/fapi/v1/order?")) {
                ++attempts;
                return http::HttpResponse{500, "{}"};
            }
            return http::HttpResponse{404, ""};
        });
        EXPECT_EQ(c.cancel_order(9101), ConnectorResult::ERROR_REST_FAILURE);
        EXPECT_EQ(attempts, 3);
        EXPECT_NE(c.venue_order_map().get(9101), nullptr);
    }

    {
        int attempts = 0;
        ScopedMockTransport transport([&](const char *method, const std::string &url,
                                          const std::string &, const std::vector<std::string> &) {
            if (std::strcmp(method, "GET") == 0 && contains(url, "/fapi/v1/order?")) {
                ++attempts;
                return http::HttpResponse{429, R"({"code":-1003,"msg":"Too many requests."})"};
            }
            return http::HttpResponse{404, ""};
        });

        FillUpdate status{};
        status.new_state = OrderState::PENDING;
        EXPECT_EQ(c.query_order(9101, status), ConnectorResult::ERROR_RATE_LIMIT);
        EXPECT_EQ(attempts, 3);
        EXPECT_EQ(status.new_state, OrderState::PENDING);
    }

    {
        ScopedMockTransport transport([&](const char *method, const std::string &url,
                                          const std::string &, const std::vector<std::string> &) {
            if (std::strcmp(method, "GET") == 0 && contains(url, "/fapi/v1/exchangeInfo?symbol=BTCUSDT"))
                return http::HttpResponse{200, exchange_info_for("BTCUSDT")};
            if (std::strcmp(method, "PUT") == 0 && contains(url, "/fapi/v1/order?"))
                return http::HttpResponse{400, R"({"code":-2022,"msg":"ReduceOnly Order is rejected."})"};
            return http::HttpResponse{404, ""};
        });

        Order replacement = make_order(9102);
        replacement.reduce_only = true;
        EXPECT_EQ(c.replace_order(9101, replacement), ConnectorResult::ERROR_INVALID_ORDER);
        EXPECT_NE(c.venue_order_map().get(9101), nullptr);
        EXPECT_EQ(c.venue_order_map().get(9102), nullptr);
    }
}

} 
