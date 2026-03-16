#include "core/common/rest_client.hpp"
#include "core/execution/binance/binance_connector.hpp"
#include "core/execution/coinbase/coinbase_connector.hpp"
#include "core/execution/kraken/kraken_connector.hpp"
#include "core/execution/okx/okx_connector.hpp"
#include "core/execution/reconciliation_service.hpp"

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

class TestableBinanceConnector : public BinanceConnector {
  public:
    using BinanceConnector::BinanceConnector;
    using BinanceConnector::fetch_reconciliation_snapshot;
};

class TestableKrakenConnector : public KrakenConnector {
  public:
    using KrakenConnector::fetch_reconciliation_snapshot;
    using KrakenConnector::KrakenConnector;
};

class TestableOkxConnector : public OkxConnector {
  public:
    using OkxConnector::fetch_reconciliation_snapshot;
    using OkxConnector::OkxConnector;
};

class TestableCoinbaseConnector : public CoinbaseConnector {
  public:
    using CoinbaseConnector::CoinbaseConnector;
    using CoinbaseConnector::fetch_reconciliation_snapshot;
};

bool contains(const std::string& s, const char* token) {
    return s.find(token) != std::string::npos;
}

http::HttpResponse binance_snapshot_response(const char* method, const std::string& url) {
    if (std::strcmp(method, "GET") != 0)
        return {404, ""};
    if (contains(url, "openOrders"))
        return {
            200,
            R"([{"orderId":"bn-1","symbol":"BTCUSDT","side":"BUY","origQty":1.0,"executedQty":0.2,"price":100.0,"status":"NEW"}])"};
    if (contains(url, "/account"))
        return {200, R"({"balances":[{"asset":"BTC","free":1.0,"locked":0.1}]})"};
    return {404, ""};
}

http::HttpResponse kraken_snapshot_response(const char* method, const std::string& url) {
    if (std::strcmp(method, "POST") != 0)
        return {404, ""};
    if (contains(url, "OpenOrders"))
        return {
            200,
            R"({"result":{"open":{"kr-1":{"status":"open","vol":1.2,"vol_exec":0.1,"descr":{"pair":"XBTUSD","type":"buy","price":100.5}}}}})"};
    if (contains(url, "Balance"))
        return {200, R"({"result":{"XXBT":2.5}})"};
    return {404, ""};
}

http::HttpResponse okx_snapshot_response(const char* method, const std::string& url) {
    if (std::strcmp(method, "GET") != 0)
        return {404, ""};
    if (contains(url, "orders-pending"))
        return {
            200,
            R"({"data":[{"ordId":"ok-1","instId":"BTC-USDT-SWAP","side":"buy","sz":3.0,"accFillSz":1.0,"px":100.0,"state":"live"}]})"};
    if (contains(url, "account/balance"))
        return {200, R"({"data":[{"details":[{"ccy":"USDT","eq":1000.0,"availEq":900.0}]}]})"};
    if (contains(url, "account/positions"))
        return {200, R"({"data":[{"instId":"BTC-USDT-SWAP","pos":1.5,"avgPx":99.5}]})"};
    return {404, ""};
}

http::HttpResponse coinbase_snapshot_response(const char* method, const std::string& url) {
    if (std::strcmp(method, "GET") != 0)
        return {404, ""};
    if (contains(url, "historical/batch"))
        return {
            200,
            R"({"orders":[{"order_id":"cb-1","product_id":"BTC-USD","side":"BUY","base_size":1.0,"filled_size":0.4,"limit_price":100.0,"status":"OPEN"}]})"};
    if (contains(url, "/accounts"))
        return {
            200,
            R"({"accounts":[{"currency":"USD","available_balance":{"value":1000.0},"hold":{"value":12.0}}]})"};
    if (contains(url, "/positions"))
        return {
            200,
            R"({"positions":[{"product_id":"BTC-USD","size":0.6,"average_entry_price":98.0}]})"};
    return {404, ""};
}

TEST(ReconciliationServiceTest, FetchSnapshotForAllConnectors) {
    ScopedMockTransport transport([](const char* method, const std::string& url, const std::string&,
                                     const std::vector<std::string>&) {
        if (contains(url, "binance.test"))
            return binance_snapshot_response(method, url);
        if (contains(url, "kraken.test"))
            return kraken_snapshot_response(method, url);
        if (contains(url, "okx.test"))
            return okx_snapshot_response(method, url);
        if (contains(url, "coinbase.test"))
            return coinbase_snapshot_response(method, url);
        return http::HttpResponse{404, ""};
    });

    TestableBinanceConnector binance("k", "s", "https://binance.test");
    TestableKrakenConnector kraken("k", "s", "https://kraken.test");
    TestableOkxConnector okx("k", "s", "https://okx.test");
    TestableCoinbaseConnector coinbase("k", "s", "https://coinbase.test");

    ReconciliationSnapshot snapshot;
    EXPECT_EQ(binance.fetch_reconciliation_snapshot(snapshot), ConnectorResult::OK);
    EXPECT_EQ(snapshot.open_orders.size, 1U);
    EXPECT_EQ(snapshot.balances.size, 1U);

    EXPECT_EQ(kraken.fetch_reconciliation_snapshot(snapshot), ConnectorResult::OK);
    EXPECT_EQ(snapshot.open_orders.size, 1U);
    EXPECT_EQ(snapshot.balances.size, 1U);

    EXPECT_EQ(okx.fetch_reconciliation_snapshot(snapshot), ConnectorResult::OK);
    EXPECT_EQ(snapshot.open_orders.size, 1U);
    EXPECT_EQ(snapshot.balances.size, 1U);
    EXPECT_EQ(snapshot.positions.size, 1U);

    EXPECT_EQ(coinbase.fetch_reconciliation_snapshot(snapshot), ConnectorResult::OK);
    EXPECT_EQ(snapshot.open_orders.size, 1U);
    EXPECT_EQ(snapshot.balances.size, 1U);
    EXPECT_EQ(snapshot.positions.size, 1U);
}

TEST(ReconciliationServiceTest, QuarantinesVenueOnDriftMismatch) {
    ScopedMockTransport transport([](const char* method, const std::string& url, const std::string&,
                                     const std::vector<std::string>&) {
        if (std::strcmp(method, "GET") != 0)
            return http::HttpResponse{404, ""};
        if (contains(url, "openOrders"))
            return http::HttpResponse{
                200,
                R"([{"orderId":"bn-1","symbol":"BTCUSDT","side":"BUY","origQty":0.5,"executedQty":0.7,"price":100.0,"status":"NEW"}])"};
        if (contains(url, "/account"))
            return http::HttpResponse{200,
                                      R"({"balances":[{"asset":"BTC","free":1.0,"locked":0.1}]})"};
        return http::HttpResponse{404, ""};
    });

    BinanceConnector binance("k", "s", "https://binance.test");
    ReconciliationService service;
    ASSERT_TRUE(service.register_connector(binance));

    EXPECT_EQ(service.reconcile_on_reconnect(), ConnectorResult::ERROR_UNKNOWN);
    EXPECT_TRUE(service.is_quarantined(Exchange::BINANCE));

    const auto* state = service.state_for(Exchange::BINANCE);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->mismatch_count, 1U);
    EXPECT_EQ(state->last_mismatch, ReconciliationService::MismatchClass::NONE);
}

TEST(ReconciliationServiceTest, UsesCanonicalSnapshotForReconnectAndPeriodicLoops) {
    ScopedMockTransport transport([](const char* method, const std::string& url, const std::string&,
                                     const std::vector<std::string>&) {
        if (contains(url, "binance.test"))
            return binance_snapshot_response(method, url);
        return http::HttpResponse{404, ""};
    });

    BinanceConnector binance("k", "s", "https://binance.test");
    ReconciliationService service;
    ASSERT_TRUE(service.register_connector(binance));

    ReconciliationSnapshot canonical;
    ReconciledOrder order;
    order.client_order_id = 11;
    std::strncpy(order.venue_order_id, "bn-1", sizeof(order.venue_order_id) - 1);
    std::strncpy(order.symbol, "BTCUSDT", sizeof(order.symbol) - 1);
    order.quantity = 1.0;
    order.filled_quantity = 0.2;
    ASSERT_TRUE(canonical.open_orders.push(order));

    ReconciledBalance balance;
    std::strncpy(balance.asset, "BTC", sizeof(balance.asset) - 1);
    balance.total = 1.1;
    balance.available = 1.0;
    ASSERT_TRUE(canonical.balances.push(balance));

    ASSERT_TRUE(service.set_canonical_snapshot(Exchange::BINANCE, canonical));

    EXPECT_EQ(service.reconcile_on_reconnect(), ConnectorResult::OK);
    EXPECT_EQ(service.run_periodic_drift_check(), ConnectorResult::OK);

    const auto* state = service.state_for(Exchange::BINANCE);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->last_mismatch, ReconciliationService::MismatchClass::NONE);
    EXPECT_EQ(state->last_action, ReconciliationService::DriftAction::NONE);
}

TEST(ReconciliationServiceTest, DetectsExplicitMismatchClasses) {
    ScopedMockTransport transport([](const char* method, const std::string& url, const std::string&,
                                     const std::vector<std::string>&) {
        if (contains(url, "binance.test"))
            return binance_snapshot_response(method, url);
        return http::HttpResponse{404, ""};
    });

    BinanceConnector binance("k", "s", "https://binance.test");
    ReconciliationService service;
    ASSERT_TRUE(service.register_connector(binance));

    ReconciliationSnapshot canonical;
    ReconciledOrder order;
    order.client_order_id = 22;
    std::strncpy(order.venue_order_id, "bn-1", sizeof(order.venue_order_id) - 1);
    std::strncpy(order.symbol, "BTCUSDT", sizeof(order.symbol) - 1);
    order.quantity = 1.0;
    order.filled_quantity = 0.2;
    ASSERT_TRUE(canonical.open_orders.push(order));

    ReconciledBalance balance;
    std::strncpy(balance.asset, "BTC", sizeof(balance.asset) - 1);
    balance.total = 9.0;
    balance.available = 8.0;
    ASSERT_TRUE(canonical.balances.push(balance));

    ASSERT_TRUE(service.set_canonical_snapshot(Exchange::BINANCE, canonical));
    EXPECT_EQ(service.reconcile_on_reconnect(), ConnectorResult::ERROR_UNKNOWN);

    const auto* state = service.state_for(Exchange::BINANCE);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->last_mismatch, ReconciliationService::MismatchClass::BALANCE_DRIFT);
    EXPECT_EQ(state->last_action,
              ReconciliationService::DriftAction::RISK_HALT_RECOMMENDED);
}

TEST(ReconciliationServiceTest, PeriodicDriftCheckSuccess) {
    ScopedMockTransport transport([](const char* method, const std::string& url, const std::string&,
                                     const std::vector<std::string>&) {
        if (contains(url, "binance.test"))
            return binance_snapshot_response(method, url);
        return http::HttpResponse{404, ""};
    });

    BinanceConnector binance("k", "s", "https://binance.test");
    ReconciliationService service;
    ASSERT_TRUE(service.register_connector(binance));

    EXPECT_EQ(service.reconcile_on_reconnect(), ConnectorResult::OK);
    EXPECT_FALSE(service.is_quarantined(Exchange::BINANCE));

    EXPECT_EQ(service.run_periodic_drift_check(), ConnectorResult::OK);

    const auto* state = service.state_for(Exchange::BINANCE);
    ASSERT_NE(state, nullptr);
    EXPECT_GT(state->last_reconcile_ts_ns, 0);
    EXPECT_GT(state->last_drift_check_ts_ns, 0);
}

} // namespace
} // namespace trading
