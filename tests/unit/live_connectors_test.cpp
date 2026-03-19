#include "core/common/rest_client.hpp"
#include "core/execution/binance/binance_connector.hpp"
#include "core/execution/coinbase/coinbase_connector.hpp"
#include "core/execution/kraken/kraken_connector.hpp"
#include "core/execution/okx/okx_connector.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <utility>

namespace trading {
namespace {

class ScopedMockTransport {
  public:
    explicit ScopedMockTransport(http::MockTransport handler) {
        http::set_mock_transport(std::move(handler));
    }
    ~ScopedMockTransport() { http::clear_mock_transport(); }
};

class TestableKrakenConnector : public KrakenConnector {
  public:
    using KrakenConnector::KrakenConnector;
    using KrakenConnector::kraken_private_headers;
};

class TestableOkxConnector : public OkxConnector {
  public:
    using OkxConnector::OkxConnector;
    using OkxConnector::auth_headers_with_timestamp;
};

bool contains(const std::string& s, const char* token) {
    return s.find(token) != std::string::npos;
}

const std::string* find_header(const std::vector<std::string>& headers, const char* prefix) {
    for (const auto& header : headers) {
        if (header.rfind(prefix, 0) == 0)
            return &header;
    }
    return nullptr;
}

Order make_order(Exchange ex, uint64_t id, const char* symbol) {
    Order o;
    o.client_order_id = id;
    o.exchange = ex;
    o.side = Side::BID;
    o.type = OrderType::LIMIT;
    o.tif = TimeInForce::IOC;
    o.price = 100.0;
    o.quantity = 0.1;
    std::strncpy(o.symbol, symbol, sizeof(o.symbol) - 1);
    o.symbol[sizeof(o.symbol) - 1] = '\0';
    return o;
}

template <typename Connector>
void run_connector_flow(Connector& c, Exchange ex, const char* symbol, const char* submit_body,
                        const char* replace_body, const char* query_body, const char* cancel_body,
                        const char* cancel_all_body) {
    ScopedMockTransport transport([submit_body, replace_body, query_body, cancel_body,
                                   cancel_all_body](const char* method, const std::string& url,
                                                    const std::string& body,
                                                    const std::vector<std::string>& headers) {
        const bool is_private_request =
            contains(url, "order") || contains(url, "Order") || contains(url, "trade");
        if (is_private_request && std::strcmp(method, "GET") != 0) {
            EXPECT_FALSE(headers.empty());
        }

        if (((std::strcmp(method, "GET") == 0) ||
             (std::strcmp(method, "POST") == 0 && contains(url, "QueryOrders"))) &&
            (contains(url, "/api/v3/order") || contains(url, "QueryOrders") ||
             contains(url, "/api/v5/trade/order") || contains(url, "historical"))) {
            return http::HttpResponse{200, query_body};
        }

        if (std::strcmp(method, "PUT") == 0 || contains(url, "EditOrder") ||
            contains(url, "AmendOrder") ||
            contains(url, "amend-order") || contains(url, "/orders/edit")) {
            return http::HttpResponse{200, replace_body};
        }

        if (std::strcmp(method, "DELETE") == 0 && contains(url, "openOrders"))
            return http::HttpResponse{200, cancel_all_body};

        if (std::strcmp(method, "DELETE") == 0 && contains(url, "/api/v3/order"))
            return http::HttpResponse{200, cancel_body};

        if (std::strcmp(method, "POST") == 0 &&
            (contains(url, "CancelOrder") || contains(url, "cancel-order"))) {
            return http::HttpResponse{200, cancel_body};
        }

        if (std::strcmp(method, "POST") == 0 &&
            (contains(url, "CancelAllOrdersAfter") || contains(url, "CancelAll")))
            return http::HttpResponse{200, cancel_all_body};

        if (std::strcmp(method, "POST") == 0 && contains(url, "cancel-batch-orders"))
            return http::HttpResponse{200, cancel_all_body};

        if (std::strcmp(method, "POST") == 0 && contains(url, "batch_cancel")) {
            if (contains(body, "order_ids"))
                return http::HttpResponse{200, cancel_body};
            if (contains(body, "product_id"))
                return http::HttpResponse{200, cancel_all_body};
        }

        if (std::strcmp(method, "POST") == 0 &&
            (contains(url, "/api/v3/order") || contains(url, "AddOrder") ||
             (contains(url, "cancel") == false && contains(url, "Cancel") == false &&
              contains(url, "order")))) {
            return http::HttpResponse{200, submit_body};
        }

        return http::HttpResponse{404, ""};
    });

    EXPECT_EQ(c.connect(), ConnectorResult::OK);

    const Order o = make_order(ex, 42, symbol);
    EXPECT_EQ(c.submit_order(o), ConnectorResult::OK);

    const VenueOrderEntry* map_entry = c.order_map().get(42);
    ASSERT_NE(map_entry, nullptr);
    EXPECT_EQ(map_entry->exchange, ex);

    FillUpdate query = {};
    EXPECT_EQ(c.query_order(42, query), ConnectorResult::OK);
    EXPECT_NE(query.new_state, OrderState::PENDING);

    Order replacement = o;
    replacement.client_order_id = 43;
    replacement.price = 101.0;
    EXPECT_EQ(c.replace_order(42, replacement), ConnectorResult::OK);
    EXPECT_EQ(c.order_map().get(42), nullptr);
    EXPECT_NE(c.order_map().get(43), nullptr);

    EXPECT_EQ(c.cancel_order(43), ConnectorResult::OK);
    EXPECT_EQ(c.order_map().get(43), nullptr);

    EXPECT_EQ(c.submit_order(o), ConnectorResult::OK);
    EXPECT_EQ(c.cancel_all(symbol), ConnectorResult::OK);

    EXPECT_EQ(c.reconcile(), ConnectorResult::OK);
}

} // namespace


TEST(LiveConnectorsTest, BinanceUsesSignedQueryContract) {
    BinanceConnector c("k", "s", "https://binance.test");
    int submit_calls = 0;
    int query_calls = 0;
    int cancel_calls = 0;
    int replace_calls = 0;
    int cancel_all_calls = 0;

    ScopedMockTransport transport([&](const char* method, const std::string& url,
                                      const std::string& body,
                                      const std::vector<std::string>& headers) {
        if (contains(url, "/api/v3/order/cancelReplace")) {
            ++replace_calls;
            EXPECT_EQ(std::strcmp(method, "POST"), 0);
            EXPECT_TRUE(body.empty());
            EXPECT_NE(find_header(headers, "X-MBX-APIKEY: "), nullptr);
            EXPECT_EQ(find_header(headers, "X-MBX-SIGNATURE: "), nullptr);
            EXPECT_TRUE(contains(url, "symbol=BTCUSDT"));
            EXPECT_TRUE(contains(url, "cancelOrderId=42"));
            EXPECT_TRUE(contains(url, "cancelReplaceMode=STOP_ON_FAILURE"));
            EXPECT_TRUE(contains(url, "newClientOrderId=TRT-43-BINANCE"));
            EXPECT_TRUE(contains(url, "price=101"));
            EXPECT_TRUE(contains(url, "timeInForce=IOC"));
            EXPECT_TRUE(contains(url, "timestamp="));
            EXPECT_TRUE(contains(url, "signature="));
            return http::HttpResponse{200,
                                      R"({"cancelResult":"SUCCESS","newOrderResponse":{"orderId":43}})"};
        }
        if (std::strcmp(method, "POST") == 0 && contains(url, "/api/v3/order?")) {
            ++submit_calls;
            EXPECT_TRUE(body.empty());
            EXPECT_NE(find_header(headers, "X-MBX-APIKEY: "), nullptr);
            EXPECT_EQ(find_header(headers, "X-MBX-SIGNATURE: "), nullptr);
            EXPECT_TRUE(contains(url, "symbol=BTCUSDT"));
            EXPECT_TRUE(contains(url, "side=BUY"));
            EXPECT_TRUE(contains(url, "type=LIMIT"));
            EXPECT_TRUE(contains(url, "timeInForce=IOC"));
            EXPECT_TRUE(contains(url, "newClientOrderId=TRT-42-BINANCE"));
            EXPECT_TRUE(contains(url, "timestamp="));
            EXPECT_TRUE(contains(url, "signature="));
            return http::HttpResponse{200, R"({"orderId":42})"};
        }
        if (std::strcmp(method, "GET") == 0 && contains(url, "/api/v3/order?")) {
            ++query_calls;
            EXPECT_TRUE(contains(url, "symbol=BTCUSDT"));
            EXPECT_TRUE(contains(url, "orderId=42"));
            EXPECT_TRUE(contains(url, "signature="));
            return http::HttpResponse{200,
                                      R"({"status":"PARTIALLY_FILLED","executedQty":"0.1","cummulativeQuoteQty":"10.1"})"};
        }
        if (std::strcmp(method, "DELETE") == 0 && contains(url, "/api/v3/order?")) {
            ++cancel_calls;
            EXPECT_TRUE(contains(url, "symbol=BTCUSDT"));
            EXPECT_TRUE(contains(url, "orderId=43"));
            EXPECT_TRUE(contains(url, "signature="));
            return http::HttpResponse{200, R"({"orderId":43,"status":"CANCELED"})"};
        }
        if (std::strcmp(method, "DELETE") == 0 && contains(url, "/api/v3/openOrders?")) {
            ++cancel_all_calls;
            EXPECT_TRUE(contains(url, "symbol=BTCUSDT"));
            EXPECT_TRUE(contains(url, "signature="));
            return http::HttpResponse{200, R"([])"};
        }
        return http::HttpResponse{404, ""};
    });

    ASSERT_EQ(c.connect(), ConnectorResult::OK);
    Order o = make_order(Exchange::BINANCE, 42, "BTCUSDT");
    ASSERT_EQ(c.submit_order(o), ConnectorResult::OK);

    FillUpdate query{};
    ASSERT_EQ(c.query_order(42, query), ConnectorResult::OK);
    EXPECT_EQ(query.new_state, OrderState::PARTIALLY_FILLED);
    EXPECT_DOUBLE_EQ(query.avg_fill_price, 101.0);

    Order replacement = o;
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

TEST(LiveConnectorsTest, BinanceRejectsUnsupportedStopLimitOrders) {
    BinanceConnector c("k", "s", "https://binance.test");
    ScopedMockTransport transport([](const char*, const std::string&, const std::string&, const std::vector<std::string>&) {
        ADD_FAILURE() << "unexpected HTTP call";
        return http::HttpResponse{500, ""};
    });

    ASSERT_EQ(c.connect(), ConnectorResult::OK);
    Order o = make_order(Exchange::BINANCE, 77, "BTCUSDT");
    o.type = OrderType::STOP_LIMIT;
    o.stop_price = 99.0;
    EXPECT_EQ(c.submit_order(o), ConnectorResult::ERROR_INVALID_ORDER);
}


TEST(LiveConnectorsTest, BinanceRejectsReplaceAcrossSymbols) {
    BinanceConnector c("k", "s", "https://binance.test");
    int submit_calls = 0;
    ScopedMockTransport transport([&](const char* method, const std::string& url,
                                      const std::string&, const std::vector<std::string>&) {
        if (std::strcmp(method, "POST") == 0 && contains(url, "/api/v3/order?")) {
            ++submit_calls;
            return http::HttpResponse{200, R"({"orderId":42})"};
        }
        ADD_FAILURE() << "unexpected HTTP call";
        return http::HttpResponse{500, ""};
    });

    ASSERT_EQ(c.connect(), ConnectorResult::OK);
    Order order = make_order(Exchange::BINANCE, 42, "BTCUSDT");
    ASSERT_EQ(c.submit_order(order), ConnectorResult::OK);

    Order replacement = order;
    replacement.client_order_id = 43;
    std::strncpy(replacement.symbol, "ETHUSDT", sizeof(replacement.symbol) - 1);
    replacement.symbol[sizeof(replacement.symbol) - 1] = '\0';
    EXPECT_EQ(c.replace_order(42, replacement), ConnectorResult::ERROR_INVALID_ORDER);
    EXPECT_EQ(submit_calls, 1);
}

TEST(LiveConnectorsTest, BinanceMapsInsufficientFundsResponses) {
    BinanceConnector c("k", "s", "https://binance.test");
    ScopedMockTransport transport([](const char* method, const std::string& url,
                                     const std::string&, const std::vector<std::string>&) {
        if (std::strcmp(method, "POST") == 0 && contains(url, "/api/v3/order?")) {
            return http::HttpResponse{400,
                                      R"({"code":-2010,"msg":"Account has insufficient balance for requested action."})"};
        }
        return http::HttpResponse{404, ""};
    });

    ASSERT_EQ(c.connect(), ConnectorResult::OK);
    EXPECT_EQ(c.submit_order(make_order(Exchange::BINANCE, 88, "BTCUSDT")),
              ConnectorResult::ERROR_INSUFFICIENT_FUNDS);
}

TEST(LiveConnectorsTest, BinanceAuthenticatedFlow) {
    BinanceConnector c("k", "s", "https://binance.test");
    run_connector_flow(c, Exchange::BINANCE, "BTCUSDT", R"({"orderId":12345})",
                       R"({"orderId":456})", R"({"status":"NEW","executedQty":0,"avgPrice":0})",
                       R"({"orderId":12345,"status":"CANCELED"})", R"([])");
}

TEST(LiveConnectorsTest, KrakenAuthenticatedFlow) {
    KrakenConnector c("k", "s", "https://kraken.test");
    run_connector_flow(c, Exchange::KRAKEN, "XBTUSD", R"({"result":{"txid":["abc-123"]}})",
                       R"({"result":{"txid":["abc-456"]}})",
                       R"({"result":{"abc-123":{"status":"open","vol_exec":0.0,"price":0.0}}})",
                       R"({"result":{"count":1}})", R"({"result":{}})");
}

TEST(LiveConnectorsTest, KrakenUsesDocumentedFormAuthAndOrderSemantics) {
    TestableKrakenConnector c(
        "pub",
        "kQH5HW/8p1uGOVjbgWA7FunAmGO8lsSUXNsu3eow76sz84Q18fWxnyRzBHCd3pd5nE9qa99HAZtuZuj6F1huXg==",
        "https://kraken.test");

    const auto example_headers = c.kraken_private_headers(
        "/0/private/AddOrder",
        "nonce=1616492376594&ordertype=limit&pair=XBTUSD&price=37500&type=buy&volume=1.25");
    const std::string* example_sign = find_header(example_headers, "API-Sign: ");
    ASSERT_NE(example_sign, nullptr);
    EXPECT_EQ(*example_sign,
              "API-Sign: "
              "4/dpxb3iT4tp/ZCVEwSnEsLxx0bqyhLpdfOpc6fn7OR8+UClSV5n9E6aSS8MPtnRfp32bAb0nmbRn6H8ndwLUQ==");

    int submit_calls = 0;
    int amend_calls = 0;
    int cancel_all_calls = 0;
    ScopedMockTransport transport([&](const char* method, const std::string& url,
                                      const std::string& body,
                                      const std::vector<std::string>& headers) {
        EXPECT_EQ(std::strcmp(method, "POST"), 0);
        EXPECT_NE(find_header(headers, "API-Key: "), nullptr);
        EXPECT_NE(find_header(headers, "API-Sign: "), nullptr);
        EXPECT_NE(find_header(headers, "Content-Type: "), nullptr);
        EXPECT_TRUE(contains(body, "nonce="));

        if (contains(url, "AddOrder")) {
            ++submit_calls;
            EXPECT_TRUE(contains(body, "pair=XBT/USD"));
            EXPECT_TRUE(contains(body, "ordertype=limit"));
            EXPECT_TRUE(contains(body, "price=100"));
            EXPECT_TRUE(contains(body, "volume=0.1"));
            EXPECT_TRUE(contains(body, "cl_ord_id=TRT-42-KRAKEN"));
            return http::HttpResponse{200, R"({"result":{"txid":["kr-42"]}})"};
        }
        if (contains(url, "QueryOrders")) {
            return http::HttpResponse{
                200, R"({"result":{"kr-42":{"status":"open","vol_exec":0.0,"price":100.0}}})"};
        }
        if (contains(url, "AmendOrder")) {
            ++amend_calls;
            EXPECT_TRUE(contains(body, "txid=kr-42"));
            EXPECT_TRUE(contains(body, "order_qty=0.1"));
            EXPECT_TRUE(contains(body, "limit_price=101"));
            EXPECT_FALSE(contains(body, "EditOrder"));
            return http::HttpResponse{200, R"({"result":{"amend_id":"ok"}})"};
        }
        if (contains(url, "CancelOrder")) {
            return http::HttpResponse{200, R"({"result":{"count":1}})"};
        }
        if (contains(url, "CancelAll")) {
            ++cancel_all_calls;
            EXPECT_FALSE(contains(url, "CancelAllOrdersAfter"));
            EXPECT_FALSE(contains(body, "pair="));
            return http::HttpResponse{200, R"({"result":{"count":2}})"};
        }
        return http::HttpResponse{404, ""};
    });

    ASSERT_EQ(c.connect(), ConnectorResult::OK);
    Order order = make_order(Exchange::KRAKEN, 42, "XBTUSD");
    ASSERT_EQ(c.submit_order(order), ConnectorResult::OK);

    FillUpdate status{};
    ASSERT_EQ(c.query_order(42, status), ConnectorResult::OK);

    Order replacement = order;
    replacement.client_order_id = 43;
    replacement.price = 101.0;
    ASSERT_EQ(c.replace_order(42, replacement), ConnectorResult::OK);
    ASSERT_NE(c.order_map().get(43), nullptr);
    EXPECT_STREQ(c.order_map().get(43)->venue_order_id, "kr-42");
    ASSERT_EQ(c.cancel_all("XBTUSD"), ConnectorResult::OK);

    EXPECT_EQ(submit_calls, 1);
    EXPECT_EQ(amend_calls, 1);
    EXPECT_EQ(cancel_all_calls, 1);
}

TEST(LiveConnectorsTest, OkxUsesDocumentedAuthHeadersAndRequestShapes) {
    TestableOkxConnector c("api-key", "api-secret", "passphrase", "https://okx.test");
    const std::string payload =
        R"({"instId":"BTC-USDT-SWAP","tdMode":"cross","clOrdId":"TRT-42-OKX","side":"buy","ordType":"ioc","sz":"0.1","px":"100"})";
    const auto headers = c.auth_headers_with_timestamp("POST", "/api/v5/trade/order", payload,
                                                       1700000000123LL, "TRT-42-OKX");
    ASSERT_NE(find_header(headers, "OK-ACCESS-KEY: "), nullptr);
    ASSERT_NE(find_header(headers, "OK-ACCESS-PASSPHRASE: "), nullptr);
    ASSERT_NE(find_header(headers, "OK-ACCESS-TIMESTAMP: "), nullptr);
    ASSERT_NE(find_header(headers, "OK-ACCESS-SIGN: "), nullptr);
    EXPECT_EQ(*find_header(headers, "OK-ACCESS-KEY: "), "OK-ACCESS-KEY: api-key");
    EXPECT_EQ(*find_header(headers, "OK-ACCESS-PASSPHRASE: "),
              "OK-ACCESS-PASSPHRASE: passphrase");
    EXPECT_EQ(*find_header(headers, "OK-ACCESS-TIMESTAMP: "),
              "OK-ACCESS-TIMESTAMP: 1700000000.123000");
    EXPECT_EQ(*find_header(headers, "OK-ACCESS-SIGN: "),
              "OK-ACCESS-SIGN: zuaFqrQF7qBRkGMYGbRa83nGaxpHMeNOTQpmxjjeIAg=");

    int submit_calls = 0;
    int query_calls = 0;
    int amend_calls = 0;
    int cancel_calls = 0;
    int cancel_all_calls = 0;
    ScopedMockTransport transport([&](const char* method, const std::string& url,
                                      const std::string& body,
                                      const std::vector<std::string>& request_headers) {
        if (std::strcmp(method, "POST") == 0 && contains(url, "/api/v5/trade/order") &&
            !contains(url, "amend-order") && !contains(url, "cancel-order")) {
            ++submit_calls;
            EXPECT_TRUE(contains(body, R"("instId":"BTC-USDT-SWAP")"));
            EXPECT_TRUE(contains(body, R"("tdMode":"cross")"));
            EXPECT_TRUE(contains(body, R"("clOrdId":"TRT-42-OKX")"));
            EXPECT_TRUE(contains(body, R"("ordType":"ioc")"));
            EXPECT_NE(find_header(request_headers, "OK-ACCESS-PASSPHRASE: "), nullptr);
            return http::HttpResponse{200, R"({"data":[{"ordId":"42"}]})"};
        }
        if (std::strcmp(method, "GET") == 0 && contains(url, "/api/v5/trade/order?")) {
            ++query_calls;
            EXPECT_TRUE(contains(url, "instId=BTC-USDT-SWAP"));
            EXPECT_TRUE(contains(url, "ordId=42"));
            EXPECT_TRUE(contains(url, "clOrdId=TRT-42-OKX"));
            return http::HttpResponse{200, R"({"data":[{"accFillSz":0.1,"avgPx":100.5,"state":"partially_filled"}]})"};
        }
        if (std::strcmp(method, "POST") == 0 && contains(url, "amend-order")) {
            ++amend_calls;
            EXPECT_TRUE(contains(body, R"("instId":"BTC-USDT-SWAP")"));
            EXPECT_TRUE(contains(body, R"("ordId":"42")"));
            EXPECT_TRUE(contains(body, R"("clOrdId":"TRT-42-OKX")"));
            EXPECT_TRUE(contains(body, R"("reqId":"TRT-43-OKX")"));
            EXPECT_TRUE(contains(body, R"("newPx":"101")"));
            EXPECT_TRUE(contains(body, R"("newSz":"0.1")"));
            return http::HttpResponse{200, R"({"data":[{"ordId":"43"}]})"};
        }
        if (std::strcmp(method, "POST") == 0 && contains(url, "cancel-order")) {
            ++cancel_calls;
            EXPECT_TRUE(contains(body, R"("instId":"BTC-USDT-SWAP")"));
            EXPECT_TRUE(contains(body, R"("ordId":"43")"));
            EXPECT_TRUE(contains(body, R"("clOrdId":"TRT-43-OKX")"));
            return http::HttpResponse{200, R"({"data":[{"sCode":"0"}]})"};
        }
        ++cancel_all_calls;
        return http::HttpResponse{500, ""};
    });

    ASSERT_EQ(c.connect(), ConnectorResult::OK);
    Order o = make_order(Exchange::OKX, 42, "BTC-USDT-SWAP");
    ASSERT_EQ(c.submit_order(o), ConnectorResult::OK);

    FillUpdate query{};
    ASSERT_EQ(c.query_order(42, query), ConnectorResult::OK);
    EXPECT_EQ(query.new_state, OrderState::PARTIALLY_FILLED);
    EXPECT_DOUBLE_EQ(query.avg_fill_price, 100.5);

    Order replacement = o;
    replacement.client_order_id = 43;
    replacement.price = 101.0;
    ASSERT_EQ(c.replace_order(42, replacement), ConnectorResult::OK);
    ASSERT_EQ(c.cancel_order(43), ConnectorResult::OK);
    EXPECT_EQ(c.cancel_all("BTC-USDT-SWAP"), ConnectorResult::ERROR_INVALID_ORDER);

    EXPECT_EQ(submit_calls, 1);
    EXPECT_EQ(query_calls, 1);
    EXPECT_EQ(amend_calls, 1);
    EXPECT_EQ(cancel_calls, 1);
    EXPECT_EQ(cancel_all_calls, 0);
}

TEST(LiveConnectorsTest, OkxRequiresPassphraseAtConnect) {
    OkxConnector c("k", "s", "", "https://okx.test");
    EXPECT_EQ(c.connect(), ConnectorResult::AUTH_FAILED);
}

TEST(LiveConnectorsTest, OkxRejectsUnsupportedStopLimitOrders) {
    OkxConnector c("k", "s", "pass", "https://okx.test");
    ScopedMockTransport transport([](const char*, const std::string&, const std::string&,
                                     const std::vector<std::string>&) {
        ADD_FAILURE() << "unexpected HTTP call";
        return http::HttpResponse{500, ""};
    });

    ASSERT_EQ(c.connect(), ConnectorResult::OK);
    Order o = make_order(Exchange::OKX, 91, "BTC-USDT-SWAP");
    o.type = OrderType::STOP_LIMIT;
    o.stop_price = 99.0;
    EXPECT_EQ(c.submit_order(o), ConnectorResult::ERROR_INVALID_ORDER);
}

TEST(LiveConnectorsTest, CoinbaseAuthenticatedFlow) {
    CoinbaseConnector c("organizations/test/apiKeys/test-key", R"(-----BEGIN EC PRIVATE KEY-----
MHcCAQEEIChMXeSk0sJJ7hQwUju2z8uVg7Vu0vCEe6F7jkFBJ7M0oAoGCCqGSM49
AwEHoUQDQgAEz5P6ZfbP2sYLVylf9g10wW9V7E+b55mzYf6z2NaE9mCVx38DSHLV
BkP4Jrped1IovnHgwlHGawEq+y3OCAXo+A==
-----END EC PRIVATE KEY-----
)", "https://coinbase.test");
    int submit_calls = 0;
    int query_calls = 0;
    int replace_calls = 0;
    int cancel_calls = 0;
    ScopedMockTransport transport([&](const char* method, const std::string& url,
                                      const std::string& body,
                                      const std::vector<std::string>& headers) {
        const std::string* auth = find_header(headers, "Authorization: Bearer ");
        if (auth == nullptr) {
            ADD_FAILURE() << "missing bearer token";
            return http::HttpResponse{500, ""};
        }
        EXPECT_NE(auth->size(), std::strlen("Authorization: Bearer "));
        EXPECT_EQ(find_header(headers, "CB-ACCESS-KEY: "), nullptr);
        EXPECT_NE(find_header(headers, "Content-Type: application/json"), nullptr);
        EXPECT_NE(find_header(headers, "Accept: application/json"), nullptr);
        if (std::strcmp(method, "POST") == 0 && contains(url, "/orders/edit")) {
            ++replace_calls;
            EXPECT_TRUE(contains(body, "\"order_id\":\"cb-42\""));
            EXPECT_TRUE(contains(body, "\"size\":\"0.1\""));
            EXPECT_TRUE(contains(body, "\"price\":\"101\""));
            return http::HttpResponse{200,
                                      R"({"success":true,"success_response":{"order_id":"cb-43"}})"};
        }
        if (std::strcmp(method, "POST") == 0 && contains(url, "/orders/batch_cancel")) {
            ++cancel_calls;
            EXPECT_TRUE(contains(body, "\"order_ids\":[\"cb-43\"]"));
            return http::HttpResponse{200, R"({"results":[{"success":true}]})"};
        }
        if (std::strcmp(method, "GET") == 0 && contains(url, "/historical/cb-42")) {
            ++query_calls;
            return http::HttpResponse{
                200, R"({"order":{"status":"OPEN","filled_size":0.0,"average_filled_price":0.0}})"};
        }
        if (std::strcmp(method, "POST") == 0 && contains(url, "/api/v3/brokerage/orders")) {
            ++submit_calls;
            EXPECT_TRUE(contains(body, "\"client_order_id\":\"42\""));
            EXPECT_TRUE(contains(body, "\"product_id\":\"BTC-USD\""));
            EXPECT_TRUE(contains(body, "\"sor_limit_ioc\""));
            EXPECT_TRUE(contains(body, "\"limit_price\":\"100\""));
            EXPECT_TRUE(contains(body, "\"base_size\":\"0.1\""));
            return http::HttpResponse{200,
                                      R"({"success":true,"success_response":{"order_id":"cb-42"}})"};
        }
        return http::HttpResponse{404, ""};
    });

    ASSERT_EQ(c.connect(), ConnectorResult::OK);
    Order order = make_order(Exchange::COINBASE, 42, "BTC-USD");
    EXPECT_EQ(c.submit_order(order), ConnectorResult::OK);

    FillUpdate query = {};
    EXPECT_EQ(c.query_order(42, query), ConnectorResult::OK);
    order.client_order_id = 43;
    order.price = 101.0;
    EXPECT_EQ(c.replace_order(42, order), ConnectorResult::OK);
    EXPECT_EQ(c.cancel_order(43), ConnectorResult::OK);
    EXPECT_EQ(submit_calls, 1);
    EXPECT_EQ(query_calls, 1);
    EXPECT_EQ(replace_calls, 1);
    EXPECT_EQ(cancel_calls, 1);
}

TEST(LiveConnectorsTest, CoinbaseRejectsUnsupportedCancelAll) {
    CoinbaseConnector c("k", "s", "https://coinbase.test");
    EXPECT_EQ(c.cancel_all("BTC-USD"), ConnectorResult::ERROR_INVALID_ORDER);
}

} // namespace trading
