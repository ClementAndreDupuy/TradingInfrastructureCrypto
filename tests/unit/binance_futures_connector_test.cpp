#include "core/common/rest_client.hpp"
#include "core/execution/binance/binance_futures_connector.hpp"

#include <gtest/gtest.h>
#include <openssl/hmac.h>

#include <cstring>
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
        EXPECT_EQ(std::strcmp(method, "POST"), 0);
        ASSERT_TRUE(contains(url, "/fapi/v1/order?"));

        const std::string query = extract_query(url);
        const std::string signature = query_value(query, "signature");
        ASSERT_FALSE(signature.empty());

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

} 
