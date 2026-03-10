#include <gtest/gtest.h>
#include "core/execution/exchange_connector.hpp"
#include "core/execution/order_manager.hpp"
#include "core/execution/binance_connector.hpp"
#include "core/execution/kraken_connector.hpp"

using namespace trading;

// ── Helpers ───────────────────────────────────────────────────────────────────

static Order make_order(uint64_t client_id,
                        const char* symbol,
                        Side side,
                        double price,
                        double qty,
                        Exchange exchange = Exchange::BINANCE) {
    Order o;
    o.client_order_id = client_id;
    std::strncpy(o.symbol, symbol, 15);
    o.exchange = exchange;
    o.side     = side;
    o.type     = OrderType::LIMIT;
    o.tif      = TimeInForce::GTC;
    o.price    = price;
    o.quantity = qty;
    o.state    = OrderState::NEW;
    return o;
}

static FillUpdate make_fill(uint64_t client_id,
                            OrderState state,
                            double fill_qty  = 0.0,
                            double fill_px   = 0.0,
                            double cum_qty   = 0.0,
                            double avg_px    = 0.0) {
    FillUpdate f;
    f.client_order_id        = client_id;
    f.new_state              = state;
    f.fill_qty               = fill_qty;
    f.fill_price             = fill_px;
    f.cumulative_filled_qty  = cum_qty;
    f.avg_fill_price         = avg_px;
    return f;
}

// ── OrderManager tests ────────────────────────────────────────────────────────

TEST(OrderManager, AddAndFindOrder) {
    OrderManager mgr;

    Order o = make_order(42, "BTCUSDT", Side::BID, 50000.0, 0.1);
    Order* slot = mgr.add_order(o);

    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(slot->client_order_id, 42u);
    EXPECT_EQ(slot->state, OrderState::SUBMITTED);

    Order* found = mgr.find_by_client_id(42);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->client_order_id, 42u);
}

TEST(OrderManager, UnknownClientIdIgnored) {
    OrderManager mgr;

    FillUpdate f = make_fill(999, OrderState::FILLED, 0.1, 50000.0, 0.1, 50000.0);
    // Must not crash
    mgr.on_fill_update(f);
}

TEST(OrderManager, FillTransitionToActive) {
    OrderManager mgr;
    Order o = make_order(1, "BTCUSDT", Side::BID, 50000.0, 0.1);
    mgr.add_order(o);

    FillUpdate f = make_fill(1, OrderState::ACTIVE);
    mgr.on_fill_update(f);

    EXPECT_EQ(mgr.find_by_client_id(1)->state, OrderState::ACTIVE);
}

TEST(OrderManager, PartialFillVwap) {
    OrderManager mgr;
    Order o = make_order(2, "BTCUSDT", Side::BID, 50000.0, 1.0);
    mgr.add_order(o);

    // First partial fill: 0.4 BTC @ 50000
    FillUpdate f1 = make_fill(2, OrderState::ACTIVE, 0.4, 50000.0, 0.4);
    mgr.on_fill_update(f1);

    Order* slot = mgr.find_by_client_id(2);
    EXPECT_DOUBLE_EQ(slot->filled_qty,    0.4);
    EXPECT_DOUBLE_EQ(slot->avg_fill_price, 50000.0);

    // Second partial fill: 0.6 BTC @ 51000
    FillUpdate f2 = make_fill(2, OrderState::FILLED, 0.6, 51000.0, 1.0);
    mgr.on_fill_update(f2);

    slot = mgr.find_by_client_id(2);
    EXPECT_DOUBLE_EQ(slot->filled_qty, 1.0);
    EXPECT_EQ(slot->state, OrderState::FILLED);

    // VWAP = (50000*0.4 + 51000*0.6) / 1.0 = 50600
    EXPECT_NEAR(slot->avg_fill_price, 50600.0, 1.0);
}

TEST(OrderManager, ExchangeAvgPriceTakesPrecedence) {
    OrderManager mgr;
    Order o = make_order(3, "BTCUSDT", Side::BID, 50000.0, 1.0);
    mgr.add_order(o);

    // Exchange reports avg_price = 50500
    FillUpdate f = make_fill(3, OrderState::FILLED, 1.0, 50000.0, 1.0, 50500.0);
    mgr.on_fill_update(f);

    EXPECT_DOUBLE_EQ(mgr.find_by_client_id(3)->avg_fill_price, 50500.0);
}

TEST(OrderManager, FillNotifyCallback) {
    OrderManager mgr;
    Order o = make_order(4, "BTCUSDT", Side::BID, 50000.0, 0.1);
    mgr.add_order(o);

    OrderState notified_state = OrderState::NEW;
    mgr.set_fill_notify([&](const Order& ord, const FillUpdate&) {
        notified_state = ord.state;
    });

    FillUpdate f = make_fill(4, OrderState::FILLED, 0.1, 50000.0, 0.1);
    mgr.on_fill_update(f);

    EXPECT_EQ(notified_state, OrderState::FILLED);
}

TEST(OrderManager, ActiveCountTracking) {
    OrderManager mgr;

    Order o1 = make_order(10, "BTCUSDT", Side::BID, 50000.0, 0.1);
    Order o2 = make_order(11, "ETHUSDT", Side::ASK, 3000.0, 1.0);
    mgr.add_order(o1);
    mgr.add_order(o2);

    EXPECT_EQ(mgr.active_count(), 2u);
    EXPECT_EQ(mgr.total_count(),  2u);

    mgr.on_fill_update(make_fill(10, OrderState::FILLED, 0.1, 50000.0, 0.1));
    EXPECT_EQ(mgr.active_count(), 1u);

    mgr.on_fill_update(make_fill(11, OrderState::CANCELED));
    EXPECT_EQ(mgr.active_count(), 0u);
}

TEST(OrderManager, ClientIdGenerator) {
    OrderManager mgr;
    uint64_t id1 = mgr.next_client_id();
    uint64_t id2 = mgr.next_client_id();
    EXPECT_LT(id1, id2);
    EXPECT_EQ(id2 - id1, 1u);
}

// ── BinanceConnector WS parsing tests ────────────────────────────────────────

TEST(BinanceConnector, ParseExecutionReportNew) {
    // Instantiate without credentials — only tests WS message parsing.
    BinanceConnector conn;

    FillUpdate received;
    conn.set_fill_callback([&](const FillUpdate& u) { received = u; });

    std::string msg = R"({
        "e": "executionReport",
        "E": 1499405658658,
        "s": "BTCUSDT",
        "c": "42",
        "S": "BUY",
        "o": "LIMIT",
        "f": "GTC",
        "q": "0.10000000",
        "p": "50000.00",
        "x": "NEW",
        "X": "NEW",
        "r": "NONE",
        "i": 12345678,
        "l": "0.00000000",
        "z": "0.00000000",
        "L": "0.00000000",
        "T": 1499405658657
    })";

    conn.process_ws_message(msg);

    EXPECT_EQ(received.client_order_id, 42u);
    EXPECT_EQ(received.new_state, OrderState::ACTIVE);
    EXPECT_DOUBLE_EQ(received.fill_qty, 0.0);
}

TEST(BinanceConnector, ParseExecutionReportPartialFill) {
    BinanceConnector conn;

    FillUpdate received;
    conn.set_fill_callback([&](const FillUpdate& u) { received = u; });

    std::string msg = R"({
        "e": "executionReport",
        "E": 1499405658658,
        "s": "BTCUSDT",
        "c": "100",
        "x": "TRADE",
        "X": "PARTIALLY_FILLED",
        "r": "NONE",
        "i": 9999,
        "l": "0.04000000",
        "z": "0.04000000",
        "L": "51000.00",
        "T": 1499405658657
    })";

    conn.process_ws_message(msg);

    EXPECT_EQ(received.client_order_id, 100u);
    EXPECT_EQ(received.new_state, OrderState::ACTIVE);
    EXPECT_NEAR(received.fill_qty,              0.04, 1e-8);
    EXPECT_NEAR(received.fill_price,         51000.0, 1e-2);
    EXPECT_NEAR(received.cumulative_filled_qty, 0.04, 1e-8);
}

TEST(BinanceConnector, ParseExecutionReportFilled) {
    BinanceConnector conn;

    FillUpdate received;
    conn.set_fill_callback([&](const FillUpdate& u) { received = u; });

    std::string msg = R"({
        "e": "executionReport",
        "c": "200",
        "x": "TRADE",
        "X": "FILLED",
        "r": "NONE",
        "i": 7777,
        "l": "0.10000000",
        "z": "0.10000000",
        "L": "50100.00",
        "T": 1499405658657
    })";

    conn.process_ws_message(msg);

    EXPECT_EQ(received.new_state, OrderState::FILLED);
    EXPECT_NEAR(received.fill_qty, 0.1, 1e-8);
}

TEST(BinanceConnector, ParseExecutionReportRejected) {
    BinanceConnector conn;

    FillUpdate received;
    conn.set_fill_callback([&](const FillUpdate& u) { received = u; });

    std::string msg = R"({
        "e": "executionReport",
        "c": "300",
        "x": "REJECTED",
        "X": "REJECTED",
        "r": "INSUFFICIENT_BALANCE",
        "i": 6666,
        "l": "0.00000000",
        "z": "0.00000000",
        "L": "0.00000000",
        "T": 1499405658657
    })";

    conn.process_ws_message(msg);

    EXPECT_EQ(received.new_state, OrderState::REJECTED);
    EXPECT_STRNE(received.reject_reason, "");
}

TEST(BinanceConnector, NonExecutionReportIgnored) {
    BinanceConnector conn;

    bool called = false;
    conn.set_fill_callback([&](const FillUpdate&) { called = true; });

    conn.process_ws_message(R"({"e":"outboundAccountPosition","E":123})");
    EXPECT_FALSE(called);
}

// ── KrakenConnector WS parsing tests ─────────────────────────────────────────

TEST(KrakenConnector, ParseExecutionUpdateNew) {
    KrakenConnector conn;

    FillUpdate received;
    conn.set_fill_callback([&](const FillUpdate& u) { received = u; });

    std::string msg = R"({
        "channel": "executions",
        "type": "update",
        "data": [{
            "order_id": "OUF4EM-FRGI2-MQMWZD",
            "cl_ord_id": "42",
            "exec_type": "new",
            "order_status": "new",
            "qty": 0.1,
            "qty_filled": 0.0,
            "last_qty": 0.0,
            "last_price": 0.0,
            "avg_price": 0.0
        }]
    })";

    conn.process_ws_message(msg);

    EXPECT_EQ(received.client_order_id, 42u);
    EXPECT_EQ(received.new_state, OrderState::ACTIVE);
    EXPECT_DOUBLE_EQ(received.fill_qty, 0.0);
}

TEST(KrakenConnector, ParseExecutionUpdateTrade) {
    KrakenConnector conn;

    FillUpdate received;
    conn.set_fill_callback([&](const FillUpdate& u) { received = u; });

    std::string msg = R"({
        "channel": "executions",
        "type": "update",
        "data": [{
            "order_id": "OUF4EM-FRGI2-MQMWZD",
            "cl_ord_id": "55",
            "exec_type": "trade",
            "order_status": "partially_filled",
            "qty_filled": 0.5,
            "last_qty": 0.5,
            "last_price": 49500.0,
            "avg_price": 49500.0
        }]
    })";

    conn.process_ws_message(msg);

    EXPECT_EQ(received.client_order_id, 55u);
    EXPECT_EQ(received.new_state, OrderState::ACTIVE);
    EXPECT_NEAR(received.fill_qty,   0.5,     1e-8);
    EXPECT_NEAR(received.fill_price, 49500.0, 1e-2);
    EXPECT_NEAR(received.avg_fill_price, 49500.0, 1e-2);
}

TEST(KrakenConnector, ParseExecutionUpdateFilled) {
    KrakenConnector conn;

    FillUpdate received;
    conn.set_fill_callback([&](const FillUpdate& u) { received = u; });

    std::string msg = R"({
        "channel": "executions",
        "type": "update",
        "data": [{
            "cl_ord_id": "77",
            "exec_type": "trade",
            "order_status": "filled",
            "qty_filled": 1.0,
            "last_qty": 0.5,
            "last_price": 50200.0,
            "avg_price": 50100.0
        }]
    })";

    conn.process_ws_message(msg);

    EXPECT_EQ(received.new_state, OrderState::FILLED);
    EXPECT_NEAR(received.avg_fill_price, 50100.0, 1e-2);
}

TEST(KrakenConnector, ParseExecutionUpdateCanceled) {
    KrakenConnector conn;

    FillUpdate received;
    conn.set_fill_callback([&](const FillUpdate& u) { received = u; });

    std::string msg = R"({
        "channel": "executions",
        "type": "update",
        "data": [{
            "cl_ord_id": "88",
            "exec_type": "canceled",
            "order_status": "canceled",
            "qty_filled": 0.0,
            "last_qty": 0.0,
            "last_price": 0.0,
            "avg_price": 0.0,
            "reason": "User requested"
        }]
    })";

    conn.process_ws_message(msg);

    EXPECT_EQ(received.new_state, OrderState::CANCELED);
}

TEST(KrakenConnector, NonExecutionChannelIgnored) {
    KrakenConnector conn;

    bool called = false;
    conn.set_fill_callback([&](const FillUpdate&) { called = true; });

    conn.process_ws_message(R"({"channel":"heartbeat","type":"update"})");
    EXPECT_FALSE(called);
}

// ── Order state helpers ───────────────────────────────────────────────────────

TEST(OrderState, StateStringMapping) {
    EXPECT_STREQ(order_state_to_string(OrderState::NEW),         "NEW");
    EXPECT_STREQ(order_state_to_string(OrderState::SUBMITTED),   "SUBMITTED");
    EXPECT_STREQ(order_state_to_string(OrderState::PENDING_NEW), "PENDING_NEW");
    EXPECT_STREQ(order_state_to_string(OrderState::ACTIVE),      "ACTIVE");
    EXPECT_STREQ(order_state_to_string(OrderState::FILLED),      "FILLED");
    EXPECT_STREQ(order_state_to_string(OrderState::CANCELED),    "CANCELED");
    EXPECT_STREQ(order_state_to_string(OrderState::REJECTED),    "REJECTED");
}
