#include "core/execution/binance/binance_connector.hpp"
#include "core/execution/coinbase/coinbase_connector.hpp"
#include "core/execution/kraken/kraken_connector.hpp"
#include "core/execution/okx/okx_connector.hpp"

#include <gtest/gtest.h>

#include <cstring>

namespace trading {
namespace {

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
void run_state_machine_contract(Connector& c, Exchange ex, const char* symbol) {
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

    // After reconcile, a fresh order should still succeed deterministically.
    const Order third = make_order<Connector>(ex, 103, symbol, Side::BID);
    EXPECT_EQ(c.submit_order(third), ConnectorResult::OK);
    ASSERT_NE(c.order_map().get(103), nullptr);
    EXPECT_EQ(c.cancel_order(103), ConnectorResult::OK);
}

}  // namespace

TEST(ConnectorContractTest, BinanceStateMachineDeterministic) {
    BinanceConnector c("", "", "mock://binance");
    run_state_machine_contract(c, Exchange::BINANCE, "BTCUSDT");
}

TEST(ConnectorContractTest, KrakenStateMachineDeterministic) {
    KrakenConnector c("", "", "mock://kraken");
    run_state_machine_contract(c, Exchange::KRAKEN, "XBTUSD");
}

TEST(ConnectorContractTest, OkxStateMachineDeterministic) {
    OkxConnector c("", "", "mock://okx");
    run_state_machine_contract(c, Exchange::OKX, "BTC-USDT-SWAP");
}

TEST(ConnectorContractTest, CoinbaseStateMachineDeterministic) {
    CoinbaseConnector c("", "", "mock://coinbase");
    run_state_machine_contract(c, Exchange::COINBASE, "BTC-USD");
}

}  // namespace trading
