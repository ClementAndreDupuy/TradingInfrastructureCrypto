#include "core/execution/market_maker.hpp"

#include <gtest/gtest.h>

#include <cstring>

namespace trading {
namespace {

class MockConnector final : public ExchangeConnector {
public:
    Exchange exchange_id() const override { return Exchange::BINANCE; }
    bool is_connected() const override { return true; }
    ConnectorResult connect() override { return ConnectorResult::OK; }
    void disconnect() override {}

    ConnectorResult submit_order(const Order& order) override {
        ++submit_count;
        last_order = order;
        return ConnectorResult::OK;
    }

    ConnectorResult cancel_order(uint64_t) override { return ConnectorResult::OK; }
    ConnectorResult cancel_all(const char*) override { return ConnectorResult::OK; }
    ConnectorResult reconcile() override { return ConnectorResult::OK; }

    int submit_count = 0;
    Order last_order{};
};

Snapshot make_snapshot() {
    Snapshot s;
    s.symbol = "BTCUSDT";
    s.exchange = Exchange::BINANCE;
    s.sequence = 1;
    s.bids.push_back({50000.0, 1.0});
    s.asks.push_back({50001.0, 1.0});
    return s;
}

}  // namespace

TEST(MarketMakerRiskTest, SubmitsQuotesWithoutCircuitBreaker) {
    MockConnector connector;
    OrderManager order_manager(connector);
    BookManager book("BTCUSDT", Exchange::BINANCE, 1.0, 20000);
    KillSwitch kill_switch;

    auto on_snapshot = book.snapshot_handler();
    on_snapshot(make_snapshot());

    MarketMakerConfig cfg;
    std::strncpy(cfg.symbol, "BTCUSDT", sizeof(cfg.symbol) - 1);
    cfg.symbol[sizeof(cfg.symbol) - 1] = '\0';

    NeuralAlphaMarketMaker maker(order_manager, book, kill_switch, nullptr, cfg);
    maker.on_book_update();

    EXPECT_GT(connector.submit_count, 0);
}

TEST(MarketMakerRiskTest, BlocksSubmitWhenCircuitBreakerRejectsRate) {
    MockConnector connector;
    OrderManager order_manager(connector);
    BookManager book("BTCUSDT", Exchange::BINANCE, 1.0, 20000);
    KillSwitch kill_switch;

    auto on_snapshot = book.snapshot_handler();
    on_snapshot(make_snapshot());

    CircuitBreakerConfig cb_cfg;
    cb_cfg.max_orders_per_second = 0;
    cb_cfg.max_orders_per_minute = 0;

    CircuitBreaker breaker(cb_cfg, kill_switch);

    MarketMakerConfig cfg;
    std::strncpy(cfg.symbol, "BTCUSDT", sizeof(cfg.symbol) - 1);
    cfg.symbol[sizeof(cfg.symbol) - 1] = '\0';

    NeuralAlphaMarketMaker maker(order_manager, book, kill_switch, &breaker, cfg);
    maker.on_book_update();

    EXPECT_EQ(connector.submit_count, 0);
}

}  // namespace trading
