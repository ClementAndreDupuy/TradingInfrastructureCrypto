#include "core/common/rest_client.hpp"
#include "core/execution/binance/binance_connector.hpp"
#include "core/execution/coinbase/coinbase_connector.hpp"
#include "core/execution/kraken/kraken_connector.hpp"
#include "core/execution/okx/okx_connector.hpp"
#include "core/execution/common/reconciliation_service.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string_view>
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

class TestReconnectConnector : public LiveConnectorBase {
  public:
    TestReconnectConnector()
        : LiveConnectorBase(Exchange::BINANCE, "k", "s", "https://binance.test") {}

    ConnectorResult reconcile() override {
        ++reconcile_calls;
        return reconcile_result;
    }

    ConnectorResult fetch_reconciliation_snapshot(ReconciliationSnapshot& snapshot) override {
        ++snapshot_calls;
        snapshot.clear();

        ReconciledOrder order;
        std::strncpy(order.venue_order_id, "bn-1", sizeof(order.venue_order_id) - 1);
        std::strncpy(order.symbol, "BTCUSDT", sizeof(order.symbol) - 1);
        order.quantity = 1.0;
        order.filled_quantity = 0.2;
        if (!snapshot.open_orders.push(order))
            return ConnectorResult::ERROR_UNKNOWN;

        ReconciledBalance balance;
        std::strncpy(balance.asset, "BTC", sizeof(balance.asset) - 1);
        balance.total = 1.1;
        balance.available = 1.0;
        if (!snapshot.balances.push(balance))
            return ConnectorResult::ERROR_UNKNOWN;

        return ConnectorResult::OK;
    }

    int reconcile_calls = 0;
    int snapshot_calls = 0;
    ConnectorResult reconcile_result = ConnectorResult::OK;

  protected:
    ConnectorResult submit_to_venue(const Order&, const std::string&, std::string&) override {
        return ConnectorResult::ERROR_UNKNOWN;
    }
    ConnectorResult cancel_at_venue(const VenueOrderEntry&) override {
        return ConnectorResult::ERROR_UNKNOWN;
    }
    ConnectorResult replace_at_venue(const VenueOrderEntry&, const Order&, std::string&) override {
        return ConnectorResult::ERROR_UNKNOWN;
    }
    ConnectorResult query_at_venue(const VenueOrderEntry&, FillUpdate&) override {
        return ConnectorResult::ERROR_UNKNOWN;
    }
    ConnectorResult cancel_all_at_venue(const char*) override { return ConnectorResult::OK; }
};

bool contains(const std::string& s, const char* token) {
    return s.find(token) != std::string::npos;
}

Order make_binance_order(uint64_t client_order_id, const char* symbol) {
    Order order{};
    order.client_order_id = client_order_id;
    order.exchange = Exchange::BINANCE;
    order.side = Side::BID;
    order.type = OrderType::LIMIT;
    order.tif = TimeInForce::IOC;
    order.price = 100.0;
    order.quantity = 1.0;
    std::strncpy(order.symbol, symbol, sizeof(order.symbol) - 1);
    order.symbol[sizeof(order.symbol) - 1] = '\0';
    return order;
}

void seed_binance_symbol(BinanceConnector& connector, uint64_t client_order_id = 101,
                         const char* symbol = "BTCUSDT") {
    ASSERT_EQ(connector.connect(), ConnectorResult::OK);
    ASSERT_EQ(connector.submit_order(make_binance_order(client_order_id, symbol)), ConnectorResult::OK);
}

http::HttpResponse binance_snapshot_response(const char* method, const std::string& url) {
    if (std::strcmp(method, "POST") == 0 && contains(url, "/api/v3/order?"))
        return {200, R"({"orderId":101})"};
    if (std::strcmp(method, "GET") != 0)
        return {404, ""};
    if (contains(url, "openOrders")) {
        EXPECT_FALSE(contains(url, "symbol="));
        return {
            200,
            R"([{"orderId":"bn-1","symbol":"BTCUSDT","clientOrderId":"TRT-101-BINANCE","side":"BUY","origQty":1.0,"executedQty":0.2,"price":100.0,"status":"NEW"}])"};
    }
    if (contains(url, "/account"))
        return {200, R"({"balances":[{"asset":"BTC","free":1.0,"locked":0.1}]})"};
    if (contains(url, "myTrades"))
        return {
            200,
            R"([{"id":901,"orderId":101,"symbol":"BTCUSDT","isBuyer":true,"qty":0.2,"price":100.0,"commission":0.0001,"commissionAsset":"BTC","time":1700000000000}])"};
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
    if (contains(url, "TradesHistory"))
        return {
            200,
            R"({"result":{"trades":{"tr-1":{"ordertxid":"kr-1","pair":"XBTUSD","type":"buy","vol":0.1,"price":100.5,"cost":10.05,"fee":0.01,"fee_ccy":"USD","time":1700000000.1}}}})"};
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
    if (contains(url, "trade/fills"))
        return {
            200,
            R"({"data":[{"tradeId":"ok-tr-1","ordId":"ok-1","instId":"BTC-USDT-SWAP","side":"buy","fillSz":1.0,"fillPx":100.0,"fee":-0.2,"feeCcy":"USDT","ts":1700000000000}]})"};
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
    if (contains(url, "/cfm/positions"))
        return {
            200,
            R"({"positions":[{"product_id":"BTC-USD","number_of_contracts":2.0,"avg_entry_price":98.0}]})"};
    if (contains(url, "historical/fills"))
        return {
            200,
            R"({"fills":[{"trade_id":"cb-tr-1","order_id":"cb-1","product_id":"BTC-USD","side":"BUY","size":0.4,"price":100.0,"commission":0.12,"commission_currency":"USD","trade_time_ns":1700000000000000000}]})"};
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
    seed_binance_symbol(binance);
    TestableKrakenConnector kraken("k", "s", "https://kraken.test");
    TestableOkxConnector okx("k", "s", "https://okx.test");
    TestableCoinbaseConnector coinbase("k", "s", "https://coinbase.test");

    ReconciliationSnapshot snapshot;
    EXPECT_EQ(binance.fetch_reconciliation_snapshot(snapshot), ConnectorResult::OK);
    EXPECT_EQ(snapshot.open_orders.size, 1U);
    EXPECT_EQ(snapshot.balances.size, 1U);
    EXPECT_EQ(snapshot.fills.size, 1U);

    EXPECT_EQ(kraken.fetch_reconciliation_snapshot(snapshot), ConnectorResult::OK);
    EXPECT_EQ(snapshot.open_orders.size, 1U);
    EXPECT_EQ(snapshot.balances.size, 1U);
    EXPECT_EQ(snapshot.fills.size, 1U);

    EXPECT_EQ(okx.fetch_reconciliation_snapshot(snapshot), ConnectorResult::OK);
    EXPECT_EQ(snapshot.open_orders.size, 1U);
    EXPECT_EQ(snapshot.balances.size, 1U);
    EXPECT_EQ(snapshot.positions.size, 1U);
    EXPECT_EQ(snapshot.fills.size, 1U);

    EXPECT_EQ(coinbase.fetch_reconciliation_snapshot(snapshot), ConnectorResult::OK);
    EXPECT_EQ(snapshot.open_orders.size, 1U);
    EXPECT_EQ(snapshot.balances.size, 1U);
    EXPECT_EQ(snapshot.positions.size, 1U);
    EXPECT_EQ(snapshot.fills.size, 1U);
}


TEST(ReconciliationServiceTest, BinanceReconciliationDerivesTradeSymbolsFromExchangeOrders) {
    ScopedMockTransport transport([](const char* method, const std::string& url, const std::string&,
                                     const std::vector<std::string>&) {
        if (std::strcmp(method, "GET") == 0 && contains(url, "/account"))
            return http::HttpResponse{200, R"({"balances":[{"asset":"USDT","free":1000.0,"locked":0.0}]})"};
        if (std::strcmp(method, "GET") == 0 && contains(url, "openOrders")) {
            EXPECT_FALSE(contains(url, "symbol="));
            return http::HttpResponse{200,
                                      R"([{"orderId":"bn-eth-1","symbol":"ETHUSDT","clientOrderId":"TRT-222-BINANCE","side":"BUY","origQty":2.0,"executedQty":0.5,"price":2500.0,"status":"NEW"}])"};
        }
        if (std::strcmp(method, "GET") == 0 && contains(url, "myTrades")) {
            EXPECT_TRUE(contains(url, "symbol=ETHUSDT"));
            return http::HttpResponse{200,
                                      R"([{"id":7001,"orderId":222,"symbol":"ETHUSDT","isBuyer":true,"qty":0.5,"price":2500.0,"commission":0.1,"commissionAsset":"USDT","time":1700000000000}])"};
        }
        return http::HttpResponse{404, ""};
    });

    TestableBinanceConnector binance("k", "s", "https://binance.test");
    ReconciliationSnapshot snapshot;
    ASSERT_EQ(binance.fetch_reconciliation_snapshot(snapshot), ConnectorResult::OK);
    ASSERT_EQ(snapshot.open_orders.size, 1U);
    EXPECT_STREQ(snapshot.open_orders.items[0].symbol, "ETHUSDT");
    EXPECT_EQ(snapshot.open_orders.items[0].client_order_id, 222U);
    ASSERT_EQ(snapshot.fills.size, 1U);
    EXPECT_STREQ(snapshot.fills.items[0].symbol, "ETHUSDT");
}

TEST(ReconciliationServiceTest, QuarantinesVenueOnDriftMismatch) {
    ScopedMockTransport transport([](const char* method, const std::string& url, const std::string&,
                                     const std::vector<std::string>&) {
        if (std::strcmp(method, "POST") == 0 && contains(url, "/api/v3/order?"))
            return http::HttpResponse{200, R"({"orderId":101})"};
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
    seed_binance_symbol(binance);
    ReconciliationService service;
    ASSERT_TRUE(service.register_connector(binance));

    EXPECT_EQ(service.reconcile_on_reconnect(), ConnectorResult::ERROR_UNKNOWN);
    EXPECT_TRUE(service.is_quarantined(Exchange::BINANCE));

    const auto* state = service.state_for(Exchange::BINANCE);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->mismatch_count, 1U);
    EXPECT_EQ(state->last_mismatch, ReconciliationService::MismatchClass::NONE);
    EXPECT_EQ(state->last_action, ReconciliationService::DriftAction::QUARANTINE_VENUE);
    EXPECT_EQ(state->last_severity, ReconciliationService::SeverityLevel::CRITICAL);
}

TEST(ReconciliationServiceTest, UsesCanonicalSnapshotForReconnectAndPeriodicLoops) {
    ScopedMockTransport transport([](const char* method, const std::string& url, const std::string&,
                                     const std::vector<std::string>&) {
        if (contains(url, "binance.test"))
            return binance_snapshot_response(method, url);
        return http::HttpResponse{404, ""};
    });

    BinanceConnector binance("k", "s", "https://binance.test");
    seed_binance_symbol(binance);
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

    ReconciledFill fill;
    fill.exchange = Exchange::BINANCE;
    fill.client_order_id = 101;
    std::strncpy(fill.venue_order_id, "101", sizeof(fill.venue_order_id) - 1);
    std::strncpy(fill.venue_trade_id, "901", sizeof(fill.venue_trade_id) - 1);
    std::strncpy(fill.symbol, "BTCUSDT", sizeof(fill.symbol) - 1);
    fill.quantity = 0.2;
    fill.price = 100.0;
    fill.notional = 20.0;
    fill.fee = 0.0001;
    fill.exchange_ts_ns = 1700000000000000000;
    ASSERT_TRUE(canonical.fills.push(fill));

    ASSERT_TRUE(service.set_canonical_snapshot(Exchange::BINANCE, canonical));

    ASSERT_TRUE(service.mark_reconnect_required(Exchange::BINANCE));
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
    seed_binance_symbol(binance);
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

    ReconciledFill fill;
    fill.exchange = Exchange::BINANCE;
    fill.client_order_id = 101;
    std::strncpy(fill.venue_order_id, "101", sizeof(fill.venue_order_id) - 1);
    std::strncpy(fill.venue_trade_id, "901", sizeof(fill.venue_trade_id) - 1);
    std::strncpy(fill.symbol, "BTCUSDT", sizeof(fill.symbol) - 1);
    fill.quantity = 0.2;
    fill.price = 100.0;
    fill.notional = 20.0;
    fill.fee = 0.0001;
    fill.exchange_ts_ns = 1700000000000000000;
    ASSERT_TRUE(canonical.fills.push(fill));

    ASSERT_TRUE(service.set_canonical_snapshot(Exchange::BINANCE, canonical));
    EXPECT_EQ(service.reconcile_on_reconnect(), ConnectorResult::ERROR_UNKNOWN);

    const auto* state = service.state_for(Exchange::BINANCE);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->last_mismatch, ReconciliationService::MismatchClass::BALANCE_DRIFT);
    EXPECT_EQ(state->last_action, ReconciliationService::DriftAction::RISK_HALT_RECOMMENDED);
}

TEST(ReconciliationServiceTest, CanonicalSnapshotFetcherIsUsedOnEachCycle) {
    ScopedMockTransport transport([](const char* method, const std::string& url, const std::string&,
                                     const std::vector<std::string>&) {
        if (contains(url, "binance.test"))
            return binance_snapshot_response(method, url);
        return http::HttpResponse{404, ""};
    });

    BinanceConnector binance("k", "s", "https://binance.test");
    seed_binance_symbol(binance);
    ReconciliationService service;
    ASSERT_TRUE(service.register_connector(binance));

    uint32_t fetch_count = 0;
    ASSERT_TRUE(service.set_canonical_snapshot_fetcher(
        Exchange::BINANCE, [&fetch_count](ReconciliationSnapshot& canonical) {
            ++fetch_count;
            canonical.clear();

            ReconciledOrder order;
            order.client_order_id = 77;
            std::strncpy(order.venue_order_id, "bn-1", sizeof(order.venue_order_id) - 1);
            std::strncpy(order.symbol, "BTCUSDT", sizeof(order.symbol) - 1);
            order.quantity = 1.0;
            order.filled_quantity = 0.2;
            if (!canonical.open_orders.push(order))
                return false;

            ReconciledBalance balance;
            std::strncpy(balance.asset, "BTC", sizeof(balance.asset) - 1);
            balance.total = 1.1;
            balance.available = 1.0;
            if (!canonical.balances.push(balance))
                return false;

            ReconciledFill fill;
            fill.exchange = Exchange::BINANCE;
            fill.client_order_id = 101;
            std::strncpy(fill.venue_order_id, "101", sizeof(fill.venue_order_id) - 1);
            std::strncpy(fill.venue_trade_id, "901", sizeof(fill.venue_trade_id) - 1);
            std::strncpy(fill.symbol, "BTCUSDT", sizeof(fill.symbol) - 1);
            fill.quantity = 0.2;
            fill.price = 100.0;
            fill.notional = 20.0;
            fill.fee = 0.0001;
            fill.exchange_ts_ns = 1700000000000000000;
            return canonical.fills.push(fill);
        }));

    EXPECT_EQ(service.reconcile_on_reconnect(), ConnectorResult::OK);
    EXPECT_EQ(service.run_periodic_drift_check(), ConnectorResult::OK);
    EXPECT_EQ(fetch_count, 2U);
}

TEST(ReconciliationServiceTest, ReconnectCycleCallsLocalReconcileBeforeSnapshotFetch) {
    TestReconnectConnector connector;
    ReconciliationService service;
    ASSERT_TRUE(service.register_connector(connector));

    ASSERT_TRUE(service.mark_reconnect_required(Exchange::BINANCE));
    EXPECT_EQ(service.reconcile_on_reconnect(), ConnectorResult::OK);
    EXPECT_EQ(connector.reconcile_calls, 1);
    EXPECT_EQ(connector.snapshot_calls, 1);

    EXPECT_EQ(service.run_periodic_drift_check(), ConnectorResult::OK);
    EXPECT_EQ(connector.reconcile_calls, 1);
    EXPECT_EQ(connector.snapshot_calls, 2);
}

TEST(ReconciliationServiceTest, ReconnectSkipsLocalReconcileWhenNotMarked) {
    TestReconnectConnector connector;
    ReconciliationService service;
    ASSERT_TRUE(service.register_connector(connector));

    EXPECT_EQ(service.reconcile_on_reconnect(), ConnectorResult::OK);
    EXPECT_EQ(connector.reconcile_calls, 0);
    EXPECT_EQ(connector.snapshot_calls, 1);
}

TEST(ReconciliationServiceTest, ReconnectFailsWhenLocalReconcileFails) {
    TestReconnectConnector connector;
    connector.reconcile_result = ConnectorResult::ERROR_REST_FAILURE;

    ReconciliationService::RemediationPolicy policy;
    policy.snapshot_failure_retry_budget = 0;

    ReconciliationService service(ReconciliationService::DriftThresholds{}, policy);
    ASSERT_TRUE(service.register_connector(connector));

    ASSERT_TRUE(service.mark_reconnect_required(Exchange::BINANCE));
    EXPECT_EQ(service.reconcile_on_reconnect(), ConnectorResult::ERROR_REST_FAILURE);
    EXPECT_EQ(connector.reconcile_calls, 1);
    EXPECT_EQ(connector.snapshot_calls, 0);
    EXPECT_TRUE(service.is_quarantined(Exchange::BINANCE));

    const auto* state = service.state_for(Exchange::BINANCE);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->last_action, ReconciliationService::DriftAction::QUARANTINE_VENUE);
    EXPECT_EQ(state->last_severity, ReconciliationService::SeverityLevel::CRITICAL);
}

TEST(ReconciliationServiceTest, PeriodicDriftCheckSuccess) {
    ScopedMockTransport transport([](const char* method, const std::string& url, const std::string&,
                                     const std::vector<std::string>&) {
        if (contains(url, "binance.test"))
            return binance_snapshot_response(method, url);
        return http::HttpResponse{404, ""};
    });

    BinanceConnector binance("k", "s", "https://binance.test");
    seed_binance_symbol(binance);
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

TEST(ReconciliationServiceTest, FillGapCheckUsesStableDedupeAndCumulativeLedger) {
    ScopedMockTransport transport([](const char* method, const std::string& url, const std::string&,
                                     const std::vector<std::string>&) {
        if (contains(url, "binance.test"))
            return binance_snapshot_response(method, url);
        return http::HttpResponse{404, ""};
    });

    ReconciliationService::DriftThresholds thresholds;
    thresholds.max_order_fill_gap = 1e-9;
    thresholds.max_fill_notional_drift = 1e-9;
    thresholds.max_fill_fee_drift = 1e-9;

    ReconciliationService::RemediationPolicy policy;
    policy.fill_gap_retry_budget = 0;

    BinanceConnector binance("k", "s", "https://binance.test");
    ReconciliationService service(thresholds, policy);
    ASSERT_TRUE(service.register_connector(binance));

    ReconciliationSnapshot canonical;

    ReconciledOrder order;
    order.client_order_id = 101;
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

    ReconciledFill fill_one;
    fill_one.exchange = Exchange::BINANCE;
    fill_one.client_order_id = 101;
    std::strncpy(fill_one.venue_order_id, "101", sizeof(fill_one.venue_order_id) - 1);
    std::strncpy(fill_one.venue_trade_id, "901", sizeof(fill_one.venue_trade_id) - 1);
    std::strncpy(fill_one.symbol, "BTCUSDT", sizeof(fill_one.symbol) - 1);
    fill_one.quantity = 0.2;
    fill_one.price = 100.0;
    fill_one.notional = 20.0;
    fill_one.fee = 0.0001;
    fill_one.exchange_ts_ns = 1700000000000000000;
    ASSERT_TRUE(canonical.fills.push(fill_one));

    ReconciledFill duplicate = fill_one;
    ASSERT_TRUE(canonical.fills.push(duplicate));

    ASSERT_TRUE(service.set_canonical_snapshot(Exchange::BINANCE, canonical));

    EXPECT_EQ(service.reconcile_on_reconnect(), ConnectorResult::OK);
    const auto* state = service.state_for(Exchange::BINANCE);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->last_mismatch, ReconciliationService::MismatchClass::NONE);
}

TEST(ReconciliationServiceTest, FillGapMismatchRequestsReplayAndQuarantines) {
    ScopedMockTransport transport([](const char* method, const std::string& url, const std::string&,
                                     const std::vector<std::string>&) {
        if (contains(url, "binance.test"))
            return binance_snapshot_response(method, url);
        return http::HttpResponse{404, ""};
    });

    ReconciliationService::DriftThresholds thresholds;
    thresholds.max_order_fill_gap = 1e-9;
    thresholds.max_fill_notional_drift = 1e-9;
    thresholds.max_fill_fee_drift = 1e-9;

    BinanceConnector binance("k", "s", "https://binance.test");
    ReconciliationService service(thresholds);
    ASSERT_TRUE(service.register_connector(binance));

    ReconciliationSnapshot canonical;

    ReconciledOrder order;
    order.client_order_id = 101;
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

    ReconciledFill fill;
    fill.exchange = Exchange::BINANCE;
    fill.client_order_id = 101;
    std::strncpy(fill.venue_order_id, "101", sizeof(fill.venue_order_id) - 1);
    std::strncpy(fill.venue_trade_id, "902", sizeof(fill.venue_trade_id) - 1);
    std::strncpy(fill.symbol, "BTCUSDT", sizeof(fill.symbol) - 1);
    fill.quantity = 0.25;
    fill.price = 100.0;
    fill.notional = 25.0;
    fill.fee = 0.0002;
    fill.exchange_ts_ns = 1700000001000000000;
    ASSERT_TRUE(canonical.fills.push(fill));

    ASSERT_TRUE(service.set_canonical_snapshot(Exchange::BINANCE, canonical));

    EXPECT_EQ(service.reconcile_on_reconnect(), ConnectorResult::ERROR_UNKNOWN);
    const auto* state = service.state_for(Exchange::BINANCE);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->last_mismatch, ReconciliationService::MismatchClass::FILL_GAP);
    EXPECT_EQ(state->last_action, ReconciliationService::DriftAction::REQUEST_FILL_REPLAY);
    EXPECT_EQ(state->fill_replay_requests, 1U);
}

TEST(ReconciliationServiceTest, SnapshotFetchFailsWhenFillIngestionFails) {
    ScopedMockTransport transport([](const char* method, const std::string& url, const std::string&,
                                     const std::vector<std::string>&) {
        if (contains(url, "binance.test")) {
            if (std::strcmp(method, "POST") == 0 && contains(url, "/api/v3/order?"))
                return http::HttpResponse{200, R"({"orderId":101})"};
            if (std::strcmp(method, "GET") != 0)
                return http::HttpResponse{404, ""};
            if (contains(url, "openOrders")) {
                return http::HttpResponse{
                    200,
                    R"([{"orderId":"bn-1","symbol":"BTCUSDT","side":"BUY","origQty":1.0,"executedQty":0.2,"price":100.0,"status":"NEW"}])"};
            }
            if (contains(url, "/account")) {
                return http::HttpResponse{
                    200, R"({"balances":[{"asset":"BTC","free":1.0,"locked":0.1}]})"};
            }
            if (contains(url, "myTrades"))
                return http::HttpResponse{503, ""};
        }
        return http::HttpResponse{404, ""};
    });

    BinanceConnector binance("k", "s", "https://binance.test");
    seed_binance_symbol(binance);
    ReconciliationService service;
    ASSERT_TRUE(service.register_connector(binance));

    EXPECT_EQ(service.reconcile_on_reconnect(), ConnectorResult::ERROR_REST_FAILURE);
    EXPECT_TRUE(service.is_quarantined(Exchange::BINANCE));
}

TEST(ReconciliationServiceTest, SnapshotAllowsMissingFillEndpoint) {
    ScopedMockTransport transport([](const char* method, const std::string& url, const std::string&,
                                     const std::vector<std::string>&) {
        if (contains(url, "binance.test")) {
            if (std::strcmp(method, "POST") == 0 && contains(url, "/api/v3/order?"))
                return http::HttpResponse{200, R"({"orderId":101})"};
            if (std::strcmp(method, "GET") != 0)
                return http::HttpResponse{404, ""};
            if (contains(url, "openOrders")) {
                return http::HttpResponse{
                    200,
                    R"([{"orderId":"bn-1","symbol":"BTCUSDT","side":"BUY","origQty":1.0,"executedQty":0.2,"price":100.0,"status":"NEW"}])"};
            }
            if (contains(url, "/account")) {
                return http::HttpResponse{
                    200, R"({"balances":[{"asset":"BTC","free":1.0,"locked":0.1}]})"};
            }
            if (contains(url, "myTrades"))
                return http::HttpResponse{404, ""};
        }
        return http::HttpResponse{404, ""};
    });

    BinanceConnector binance("k", "s", "https://binance.test");
    seed_binance_symbol(binance);
    ReconciliationService service;
    ASSERT_TRUE(service.register_connector(binance));

    EXPECT_EQ(service.reconcile_on_reconnect(), ConnectorResult::OK);
    EXPECT_FALSE(service.is_quarantined(Exchange::BINANCE));
}

TEST(ReconciliationServiceTest, StagedRemediationEscalatesOrderDriftToRiskHalt) {
    ScopedMockTransport transport([](const char* method, const std::string& url, const std::string&,
                                     const std::vector<std::string>&) {
        if (std::strcmp(method, "POST") == 0 && contains(url, "/api/v3/order?"))
            return http::HttpResponse{200, R"({"orderId":101})"};
        if (std::strcmp(method, "GET") != 0)
            return http::HttpResponse{404, ""};
        if (contains(url, "openOrders"))
            return http::HttpResponse{
                200,
                R"([{"orderId":"bn-1","symbol":"BTCUSDT","side":"BUY","origQty":0.5,"executedQty":0.3,"price":100.0,"status":"NEW"}])"};
        if (contains(url, "/account"))
            return http::HttpResponse{200,
                                      R"({"balances":[{"asset":"BTC","free":1.0,"locked":0.1}]})"};
        return http::HttpResponse{404, ""};
    });

    ReconciliationService::RemediationPolicy policy;
    policy.order_drift_retry_budget = 1;
    BinanceConnector binance("k", "s", "https://binance.test");
    ReconciliationService service(ReconciliationService::DriftThresholds{}, policy);
    ASSERT_TRUE(service.register_connector(binance));

    ReconciliationSnapshot canonical;
    ReconciledOrder order;
    std::strncpy(order.venue_order_id, "bn-1", sizeof(order.venue_order_id) - 1);
    std::strncpy(order.symbol, "BTCUSDT", sizeof(order.symbol) - 1);
    order.quantity = 0.5;
    order.filled_quantity = 0.2;
    order.price = 100.0;
    ASSERT_TRUE(canonical.open_orders.push(order));

    ReconciledBalance balance;
    std::strncpy(balance.asset, "BTC", sizeof(balance.asset) - 1);
    balance.total = 1.1;
    balance.available = 1.0;
    ASSERT_TRUE(canonical.balances.push(balance));

    ASSERT_TRUE(service.set_canonical_snapshot(Exchange::BINANCE, canonical));

    uint32_t cancel_all_calls = 0;
    uint32_t risk_halt_calls = 0;
    service.set_cancel_all_hook([&](Exchange, ReconciliationService::MismatchClass,
                                    std::string_view) { ++cancel_all_calls; });
    service.set_risk_halt_hook([&](Exchange, ReconciliationService::MismatchClass,
                                   std::string_view) { ++risk_halt_calls; });

    EXPECT_EQ(service.reconcile_on_reconnect(), ConnectorResult::ERROR_UNKNOWN);
    EXPECT_FALSE(service.is_quarantined(Exchange::BINANCE));
    EXPECT_EQ(cancel_all_calls, 1U);
    EXPECT_EQ(risk_halt_calls, 0U);

    EXPECT_EQ(service.run_periodic_drift_check(), ConnectorResult::ERROR_UNKNOWN);
    EXPECT_TRUE(service.is_quarantined(Exchange::BINANCE));
    EXPECT_EQ(cancel_all_calls, 1U);
    EXPECT_EQ(risk_halt_calls, 1U);

    const auto* state = service.state_for(Exchange::BINANCE);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->last_mismatch, ReconciliationService::MismatchClass::NONE);
    EXPECT_EQ(state->last_action, ReconciliationService::DriftAction::RISK_HALT_RECOMMENDED);
    EXPECT_EQ(state->last_severity, ReconciliationService::SeverityLevel::CRITICAL);

    size_t incident_count = 0;
    const auto* incidents = service.incident_trail(incident_count);
    ASSERT_NE(incidents, nullptr);
    ASSERT_EQ(incident_count, 2U);
    EXPECT_EQ(incidents[0].action, ReconciliationService::DriftAction::CANCEL_ALL_ORDERS);
    EXPECT_EQ(incidents[0].severity, ReconciliationService::SeverityLevel::WARNING);
    EXPECT_EQ(incidents[1].action, ReconciliationService::DriftAction::RISK_HALT_RECOMMENDED);
    EXPECT_EQ(incidents[1].severity, ReconciliationService::SeverityLevel::CRITICAL);
}

TEST(ReconciliationServiceTest, SnapshotFailureUsesRetryBudgetThenQuarantine) {
    ScopedMockTransport transport([](const char* method, const std::string& url, const std::string&,
                                     const std::vector<std::string>&) {
        if (contains(url, "binance.test") && contains(url, "openOrders"))
            return http::HttpResponse{503, ""};
        if (std::strcmp(method, "GET") == 0 && contains(url, "/account"))
            return http::HttpResponse{200,
                                      R"({"balances":[{"asset":"BTC","free":1.0,"locked":0.1}]})"};
        return http::HttpResponse{404, ""};
    });

    ReconciliationService::RemediationPolicy policy;
    policy.snapshot_failure_retry_budget = 1;

    BinanceConnector binance("k", "s", "https://binance.test");
    ReconciliationService service(ReconciliationService::DriftThresholds{}, policy);
    ASSERT_TRUE(service.register_connector(binance));

    uint32_t cancel_all_calls = 0;
    service.set_cancel_all_hook([&](Exchange, ReconciliationService::MismatchClass,
                                    std::string_view) { ++cancel_all_calls; });

    EXPECT_EQ(service.reconcile_on_reconnect(), ConnectorResult::ERROR_REST_FAILURE);
    EXPECT_FALSE(service.is_quarantined(Exchange::BINANCE));
    EXPECT_EQ(cancel_all_calls, 1U);

    EXPECT_EQ(service.run_periodic_drift_check(), ConnectorResult::ERROR_REST_FAILURE);
    EXPECT_TRUE(service.is_quarantined(Exchange::BINANCE));

    const auto* state = service.state_for(Exchange::BINANCE);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->last_action, ReconciliationService::DriftAction::QUARANTINE_VENUE);
    EXPECT_EQ(state->last_severity, ReconciliationService::SeverityLevel::CRITICAL);
}

TEST(ReconciliationServiceTest, AuthSnapshotFailureQuarantinesImmediately) {
    ScopedMockTransport transport([](const char* method, const std::string& url, const std::string&,
                                     const std::vector<std::string>&) {
        if (contains(url, "binance.test") && contains(url, "openOrders"))
            return http::HttpResponse{401, ""};
        if (std::strcmp(method, "GET") == 0 && contains(url, "/account"))
            return http::HttpResponse{200,
                                      R"({"balances":[{"asset":"BTC","free":1.0,"locked":0.1}]})"};
        return http::HttpResponse{404, ""};
    });

    ReconciliationService::RemediationPolicy policy;
    policy.snapshot_failure_retry_budget = 5;

    BinanceConnector binance("k", "s", "https://binance.test");
    ReconciliationService service(ReconciliationService::DriftThresholds{}, policy);
    ASSERT_TRUE(service.register_connector(binance));

    uint32_t cancel_all_calls = 0;
    service.set_cancel_all_hook([&](Exchange, ReconciliationService::MismatchClass,
                                    std::string_view) { ++cancel_all_calls; });

    EXPECT_EQ(service.reconcile_on_reconnect(), ConnectorResult::AUTH_FAILED);
    EXPECT_TRUE(service.is_quarantined(Exchange::BINANCE));
    EXPECT_EQ(cancel_all_calls, 0U);

    const auto* state = service.state_for(Exchange::BINANCE);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->last_action, ReconciliationService::DriftAction::QUARANTINE_VENUE);
    EXPECT_EQ(state->last_severity, ReconciliationService::SeverityLevel::CRITICAL);
}

} // namespace
} // namespace trading
