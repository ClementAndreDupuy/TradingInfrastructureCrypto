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

bool contains(const std::string& s, const char* token) {
    return s.find(token) != std::string::npos;
}

bool is_submit_request(const char* method, const std::string& url) {
    return std::strcmp(method, "POST") == 0 &&
           (contains(url, "/api/v3/order") || contains(url, "AddOrder") ||
            (contains(url, "cancel") == false && contains(url, "Cancel") == false &&
             contains(url, "order")));
}

bool is_cancel_request(const char* method, const std::string& url, const std::string& body) {
    if (std::strcmp(method, "DELETE") == 0 &&
        (contains(url, "/api/v3/order") || contains(url, "openOrders")))
        return true;
    if (std::strcmp(method, "POST") == 0 &&
        (contains(url, "CancelOrder") || contains(url, "cancel-order") ||
         contains(url, "cancel-batch-orders") || contains(url, "CancelAllOrdersAfter")))
        return true;
    return std::strcmp(method, "POST") == 0 && contains(url, "batch_cancel") &&
           contains(body, "order_ids");
}

bool is_replace_request(const char* method, const std::string& url) {
    return std::strcmp(method, "PUT") == 0 || contains(url, "EditOrder") ||
           contains(url, "amend-order") || contains(url, "/orders/edit");
}

bool is_query_request(const char* method, const std::string& url) {
    return ((std::strcmp(method, "GET") == 0) ||
            (std::strcmp(method, "POST") == 0 && contains(url, "QueryOrders"))) &&
           (contains(url, "/api/v3/order") || contains(url, "QueryOrders") ||
            contains(url, "/api/v5/trade/order") || contains(url, "historical"));
}

Order make_order(Exchange ex, uint64_t id, const char* symbol) {
    Order o{};
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
void run_submit_classification_suite(Connector& c, Exchange exchange, const char* symbol) {
    struct SubmitCase {
        int status;
        ConnectorResult expected;
        int expected_attempts;
    };

    const SubmitCase cases[] = {
        {401, ConnectorResult::AUTH_FAILED, 1},
        {429, ConnectorResult::ERROR_RATE_LIMIT, 3},
        {500, ConnectorResult::ERROR_REST_FAILURE, 3},
        {400, ConnectorResult::ERROR_INVALID_ORDER, 1},
        {0, ConnectorResult::ERROR_UNKNOWN, 3},
    };

    uint64_t id = 900000;
    for (const SubmitCase& tc : cases) {
        int attempts = 0;
        ScopedMockTransport transport([&](const char* method, const std::string& url,
                                          const std::string&, const std::vector<std::string>&) {
            if (is_submit_request(method, url)) {
                ++attempts;
                return http::HttpResponse{tc.status, "{}"};
            }
            return http::HttpResponse{404, ""};
        });

        const Order order = make_order(exchange, ++id, symbol);
        EXPECT_EQ(c.submit_order(order), tc.expected);
        EXPECT_EQ(attempts, tc.expected_attempts);
        EXPECT_EQ(c.venue_order_map().get(order.client_order_id), nullptr);
    }
}

template <typename Connector>
void run_operation_state_invariant_suite(Connector& c, Exchange exchange, const char* symbol,
                                         const char* submit_ok_body) {
    uint64_t id = 910000;
    {
        ScopedMockTransport transport([&](const char* method, const std::string& url,
                                          const std::string&, const std::vector<std::string>&) {
            if (is_submit_request(method, url))
                return http::HttpResponse{200, submit_ok_body};
            return http::HttpResponse{404, ""};
        });

        const Order seed = make_order(exchange, ++id, symbol);
        ASSERT_EQ(c.submit_order(seed), ConnectorResult::OK);
    }

    const uint64_t current_id = id;

    {
        int attempts = 0;
        ScopedMockTransport transport([&](const char* method, const std::string& url,
                                          const std::string& body,
                                          const std::vector<std::string>&) {
            if (is_cancel_request(method, url, body)) {
                ++attempts;
                return http::HttpResponse{500, "{}"};
            }
            return http::HttpResponse{404, ""};
        });

        EXPECT_EQ(c.cancel_order(current_id), ConnectorResult::ERROR_REST_FAILURE);
        EXPECT_EQ(attempts, 3);
        EXPECT_NE(c.venue_order_map().get(current_id), nullptr);
    }

    {
        int attempts = 0;
        ScopedMockTransport transport([&](const char* method, const std::string& url,
                                          const std::string&, const std::vector<std::string>&) {
            if (is_replace_request(method, url)) {
                ++attempts;
                return http::HttpResponse{401, "{}"};
            }
            return http::HttpResponse{404, ""};
        });

        Order replacement = make_order(exchange, ++id, symbol);
        EXPECT_EQ(c.replace_order(current_id, replacement), ConnectorResult::AUTH_FAILED);
        EXPECT_EQ(attempts, 1);
        EXPECT_NE(c.venue_order_map().get(current_id), nullptr);
        EXPECT_EQ(c.venue_order_map().get(replacement.client_order_id), nullptr);
    }

    {
        int attempts = 0;
        ScopedMockTransport transport([&](const char* method, const std::string& url,
                                          const std::string&, const std::vector<std::string>&) {
            if (is_query_request(method, url)) {
                ++attempts;
                return http::HttpResponse{429, "{}"};
            }
            return http::HttpResponse{404, ""};
        });

        FillUpdate status{};
        status.new_state = OrderState::PENDING;
        EXPECT_EQ(c.query_order(current_id, status), ConnectorResult::ERROR_RATE_LIMIT);
        EXPECT_EQ(attempts, 3);
        EXPECT_EQ(status.new_state, OrderState::PENDING);
    }
}

TEST(ConnectorFailureInjectionTest, BinanceDeterministicFailures) {
    BinanceConnector c("k", "s", "https://binance.test");
    ASSERT_EQ(c.connect(), ConnectorResult::OK);
    run_submit_classification_suite(c, Exchange::BINANCE, "BTCUSDT");
    run_operation_state_invariant_suite(c, Exchange::BINANCE, "BTCUSDT", R"({"orderId":"bn-1"})");
}

TEST(ConnectorFailureInjectionTest, KrakenDeterministicFailures) {
    KrakenConnector c("k", "s", "https://kraken.test");
    ASSERT_EQ(c.connect(), ConnectorResult::OK);
    run_submit_classification_suite(c, Exchange::KRAKEN, "XBTUSD");
    run_operation_state_invariant_suite(c, Exchange::KRAKEN, "XBTUSD",
                                        R"({"result":{"txid":["kr-1"]}})");
}

TEST(ConnectorFailureInjectionTest, OkxDeterministicFailures) {
    OkxConnector c("k", "s", "pass", "https://okx.test");
    ASSERT_EQ(c.connect(), ConnectorResult::OK);
    run_submit_classification_suite(c, Exchange::OKX, "BTC-USDT-SWAP");
    run_operation_state_invariant_suite(c, Exchange::OKX, "BTC-USDT-SWAP",
                                        R"({"data":[{"ordId":"ok-1"}]})");
}

TEST(ConnectorFailureInjectionTest, CoinbaseDeterministicFailures) {
    CoinbaseConnector c("k", "s", "https://coinbase.test");
    ASSERT_EQ(c.connect(), ConnectorResult::OK);
    run_submit_classification_suite(c, Exchange::COINBASE, "BTC-USD");
    run_operation_state_invariant_suite(c, Exchange::COINBASE, "BTC-USD",
                                        R"({"success_response":{"order_id":"cb-1"}})");
}

} // namespace
} // namespace trading
