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

template <typename Connector>
Order make_order(Exchange ex, uint64_t id, const char* symbol, Side side = Side::BID) {
    Order o{};
    o.client_order_id = id;
    o.exchange = ex;
    o.side = side;
    o.type = OrderType::LIMIT;
    o.tif = TimeInForce::IOC;
    o.price = 100.0 + static_cast<double>(id);
    o.quantity = 0.1;
    std::strncpy(o.symbol, symbol, sizeof(o.symbol) - 1);
    o.symbol[sizeof(o.symbol) - 1] = '\0';
    return o;
}

template <typename Connector>
void run_state_machine_contract(Connector& c, Exchange ex, const char* symbol,
                                const char* submit_body, const char* cancel_body) {
    ScopedMockTransport transport(
        [submit_body, cancel_body](const char* method, const std::string& url, const std::string&,
                                   const std::vector<std::string>&) {
            if (std::strcmp(method, "POST") == 0 && url.find("order") != std::string::npos)
                return http::HttpResponse{200, submit_body};
            if (std::strcmp(method, "DELETE") == 0 ||
                (std::strcmp(method, "POST") == 0 && url.find("cancel") != std::string::npos) ||
                (std::strcmp(method, "POST") == 0 && url.find("Cancel") != std::string::npos)) {
                return http::HttpResponse{200, cancel_body};
            }
            return http::HttpResponse{404, ""};
        });

    EXPECT_EQ(c.connect(), ConnectorResult::OK);

    const Order first = make_order<Connector>(ex, 101, symbol, Side::BID);
    const Order second = make_order<Connector>(ex, 102, symbol, Side::ASK);

    EXPECT_EQ(c.submit_order(first), ConnectorResult::OK);
    EXPECT_EQ(c.submit_order(second), ConnectorResult::OK);
    ASSERT_NE(c.order_map().get(101), nullptr);
    ASSERT_NE(c.order_map().get(102), nullptr);

    EXPECT_EQ(c.cancel_order(101), ConnectorResult::OK);
    EXPECT_EQ(c.order_map().get(101), nullptr);
    ASSERT_NE(c.order_map().get(102), nullptr);

    EXPECT_EQ(c.cancel_order(999999), ConnectorResult::ERROR_INVALID_ORDER);

    EXPECT_EQ(c.reconcile(), ConnectorResult::OK);
    EXPECT_EQ(c.order_map().get(102), nullptr);

    const Order third = make_order<Connector>(ex, 103, symbol, Side::BID);
    EXPECT_EQ(c.submit_order(third), ConnectorResult::OK);
    ASSERT_NE(c.order_map().get(103), nullptr);
    EXPECT_EQ(c.cancel_order(103), ConnectorResult::OK);
}

} // namespace

TEST(ConnectorContractTest, BinanceStateMachineDeterministic) {
    BinanceConnector c("", "", "https://binance.test");
    run_state_machine_contract(c, Exchange::BINANCE, "BTCUSDT", R"({"orderId":"bn-101"})",
                               R"({"orderId":"bn-101","status":"CANCELED"})");
}

TEST(ConnectorContractTest, KrakenStateMachineDeterministic) {
    KrakenConnector c("", "", "https://kraken.test");
    run_state_machine_contract(c, Exchange::KRAKEN, "XBTUSD", R"({"result":{"txid":["kr-101"]}})",
                               R"({"result":{"count":1}})");
}

TEST(ConnectorContractTest, OkxStateMachineDeterministic) {
    OkxConnector c("", "", "https://okx.test");
    run_state_machine_contract(c, Exchange::OKX, "BTC-USDT-SWAP",
                               R"({"data":[{"ordId":"ok-101"}]})", R"({"data":[{"sCode":"0"}]})");
}

TEST(ConnectorContractTest, CoinbaseStateMachineDeterministic) {
    CoinbaseConnector c("", "", "https://coinbase.test");
    run_state_machine_contract(c, Exchange::COINBASE, "BTC-USD",
                               R"({"success_response":{"order_id":"cb-101"}})",
                               R"({"results":[{"success":true}]})");
}

TEST(ConnectorContractTest, RejectsMalformedVenueResponses) {
    ScopedMockTransport transport([](const char* method, const std::string&, const std::string&,
                                     const std::vector<std::string>&) {
        if (std::strcmp(method, "POST") == 0)
            return http::HttpResponse{200, "{}"};
        return http::HttpResponse{200, "{}"};
    });

    BinanceConnector c("", "", "https://binance.test");
    EXPECT_EQ(c.connect(), ConnectorResult::OK);

    const Order o = make_order<BinanceConnector>(Exchange::BINANCE, 501, "BTCUSDT");
    EXPECT_EQ(c.submit_order(o), ConnectorResult::ERROR_UNKNOWN);
}

} // namespace trading
