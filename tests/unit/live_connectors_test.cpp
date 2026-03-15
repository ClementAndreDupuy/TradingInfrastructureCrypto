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
    ScopedMockTransport transport([submit_body, replace_body, query_body, cancel_body, cancel_all_body](
                                      const char* method, const std::string& url,
                                      const std::string& body,
                                      const std::vector<std::string>& headers) {
        const bool is_private_request =
            contains(url, "order") || contains(url, "Order") || contains(url, "trade");
        if (is_private_request && std::strcmp(method, "GET") != 0)
            EXPECT_FALSE(headers.empty());

        if (std::strcmp(method, "GET") == 0 &&
            (contains(url, "/api/v3/order") || contains(url, "QueryOrders") ||
             contains(url, "/api/v5/trade/order") || contains(url, "historical"))) {
            return http::HttpResponse{200, query_body};
        }

        if (std::strcmp(method, "PUT") == 0 || contains(url, "EditOrder") ||
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

        if (std::strcmp(method, "POST") == 0 && contains(url, "CancelAllOrdersAfter"))
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

TEST(LiveConnectorsTest, BinanceAuthenticatedFlow) {
    BinanceConnector c("k", "s", "https://binance.test");
    run_connector_flow(c, Exchange::BINANCE, "BTCUSDT", R"({"orderId":"12345"})",
                       R"({"orderId":"456"})",
                       R"({"status":"NEW","executedQty":0,"avgPrice":0})",
                       R"({"orderId":"12345","status":"CANCELED"})", R"([])");
}

TEST(LiveConnectorsTest, KrakenAuthenticatedFlow) {
    KrakenConnector c("k", "s", "https://kraken.test");
    run_connector_flow(c, Exchange::KRAKEN, "XBTUSD", R"({"result":{"txid":["abc-123"]}})",
                       R"({"result":{"txid":["abc-456"]}})",
                       R"({"result":{"abc-123":{"status":"open","vol_exec":0.0,"price":0.0}}})",
                       R"({"result":{"count":1}})", R"({"result":{}})");
}

TEST(LiveConnectorsTest, OkxAuthenticatedFlow) {
    OkxConnector c("k", "s", "https://okx.test");
    run_connector_flow(c, Exchange::OKX, "BTC-USDT-SWAP", R"({"data":[{"ordId":"42"}]})",
                       R"({"data":[{"ordId":"43"}]})",
                       R"({"data":[{"state":"live","accFillSz":0,"avgPx":0}]})",
                       R"({"data":[{"sCode":"0"}]})", R"({"data":[{"sCode":"0"}]})");
}

TEST(LiveConnectorsTest, CoinbaseAuthenticatedFlow) {
    CoinbaseConnector c("k", "s", "https://coinbase.test");
    run_connector_flow(c, Exchange::COINBASE, "BTC-USD",
                       R"({"success_response":{"order_id":"cb-42"}})",
                       R"({"success_response":{"order_id":"cb-43"}})",
                       R"({"order":{"status":"OPEN","filled_size":0,"average_filled_price":0}})",
                       R"({"results":[{"success":true}]})", R"({"results":[{"success":true}]})");
}

} // namespace trading
