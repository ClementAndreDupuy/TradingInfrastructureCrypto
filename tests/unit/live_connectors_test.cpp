#include "core/execution/binance/binance_connector.hpp"
#include "core/execution/coinbase/coinbase_connector.hpp"
#include "core/execution/kraken/kraken_connector.hpp"
#include "core/execution/okx/okx_connector.hpp"

#include <gtest/gtest.h>

#include <cstring>

namespace trading {
namespace {

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
void run_connector_flow(Connector& c, Exchange ex, const char* symbol) {
    EXPECT_EQ(c.connect(), ConnectorResult::OK);

    const Order o = make_order(ex, 42, symbol);
    EXPECT_EQ(c.submit_order(o), ConnectorResult::OK);

    const VenueOrderEntry* map_entry = c.order_map().get(42);
    ASSERT_NE(map_entry, nullptr);
    EXPECT_EQ(map_entry->exchange, ex);

    EXPECT_EQ(c.cancel_order(42), ConnectorResult::OK);
    EXPECT_EQ(c.order_map().get(42), nullptr);

    EXPECT_EQ(c.reconcile(), ConnectorResult::OK);
}

} // namespace

TEST(LiveConnectorsTest, BinanceMockFlow) {
    BinanceConnector c("", "", "mock://binance");
    run_connector_flow(c, Exchange::BINANCE, "BTCUSDT");
}

TEST(LiveConnectorsTest, KrakenMockFlow) {
    KrakenConnector c("", "", "mock://kraken");
    run_connector_flow(c, Exchange::KRAKEN, "XBTUSD");
}

TEST(LiveConnectorsTest, OkxMockFlow) {
    OkxConnector c("", "", "mock://okx");
    run_connector_flow(c, Exchange::OKX, "BTC-USDT-SWAP");
}

TEST(LiveConnectorsTest, CoinbaseMockFlow) {
    CoinbaseConnector c("", "", "mock://coinbase");
    run_connector_flow(c, Exchange::COINBASE, "BTC-USD");
}

} // namespace trading
