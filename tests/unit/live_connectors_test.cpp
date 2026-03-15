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
    explicit ScopedMockTransport(http::MockTransport handler) { http::set_mock_transport(std::move(handler)); }
    ~ScopedMockTransport() { http::clear_mock_transport(); }
};

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
void run_connector_flow(Connector& c, Exchange ex, const char* symbol,
                        const char* submit_body, const char* cancel_body,
                        const char* cancel_all_body) {
    ScopedMockTransport transport([submit_body, cancel_body, cancel_all_body](
                                      const char* method, const std::string& url,
                                      const std::string&, const std::vector<std::string>&) {
        if (std::strcmp(method, "POST") == 0 && url.find("order") != std::string::npos)
            return http::HttpResponse{200, submit_body};
        if (std::strcmp(method, "DELETE") == 0 && url.find("openOrders") != std::string::npos)
            return http::HttpResponse{200, cancel_all_body};
        if ((std::strcmp(method, "DELETE") == 0 && url.find("order") != std::string::npos) ||
            (std::strcmp(method, "POST") == 0 && url.find("Cancel") != std::string::npos) ||
            (std::strcmp(method, "POST") == 0 && url.find("cancel") != std::string::npos)) {
            return http::HttpResponse{200, cancel_body};
        }
        return http::HttpResponse{404, ""};
    });

    EXPECT_EQ(c.connect(), ConnectorResult::OK);

    const Order o = make_order(ex, 42, symbol);
    EXPECT_EQ(c.submit_order(o), ConnectorResult::OK);

    const VenueOrderEntry* map_entry = c.order_map().get(42);
    ASSERT_NE(map_entry, nullptr);
    EXPECT_EQ(map_entry->exchange, ex);

    EXPECT_EQ(c.cancel_order(42), ConnectorResult::OK);
    EXPECT_EQ(c.order_map().get(42), nullptr);

    EXPECT_EQ(c.submit_order(o), ConnectorResult::OK);
    EXPECT_EQ(c.cancel_all(symbol), ConnectorResult::OK);

    EXPECT_EQ(c.reconcile(), ConnectorResult::OK);
}

} // namespace

TEST(LiveConnectorsTest, BinanceAuthenticatedFlow) {
    BinanceConnector c("k", "s", "https://binance.test");
    run_connector_flow(c, Exchange::BINANCE, "BTCUSDT", R"({"orderId":"12345"})",
                       R"({"orderId":"12345","status":"CANCELED"})", R"([])");
}

TEST(LiveConnectorsTest, KrakenAuthenticatedFlow) {
    KrakenConnector c("k", "s", "https://kraken.test");
    run_connector_flow(c, Exchange::KRAKEN, "XBTUSD", R"({"result":{"txid":["abc-123"]}})",
                       R"({"result":{"count":1}})", R"({"result":{}})");
}

TEST(LiveConnectorsTest, OkxAuthenticatedFlow) {
    OkxConnector c("k", "s", "https://okx.test");
    run_connector_flow(c, Exchange::OKX, "BTC-USDT-SWAP", R"({"data":[{"ordId":"42"}]})",
                       R"({"data":[{"sCode":"0"}]})", R"({"data":[{"sCode":"0"}]})");
}

TEST(LiveConnectorsTest, CoinbaseAuthenticatedFlow) {
    CoinbaseConnector c("k", "s", "https://coinbase.test");
    run_connector_flow(c, Exchange::COINBASE, "BTC-USD",
                       R"({"success_response":{"order_id":"cb-42"}})",
                       R"({"results":[{"success":true}]})",
                       R"({"results":[{"success":true}]})");
}

} // namespace trading
