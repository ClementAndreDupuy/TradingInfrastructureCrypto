#include "core/execution/live_connector_base.hpp"
#include "core/risk/recovery_guard.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace trading {
namespace {

class FakeConnector : public LiveConnectorBase {
  public:
    explicit FakeConnector(Exchange ex)
        : LiveConnectorBase(ex, "k", "s", "https://test", RetryPolicy{3, 0}) {}

    ConnectorResult submit_result = ConnectorResult::OK;
    ConnectorResult cancel_result = ConnectorResult::OK;
    ConnectorResult replace_result = ConnectorResult::OK;

    int submit_calls = 0;
    int cancel_calls = 0;
    int replace_calls = 0;
    std::string next_venue_order_id = "venue-1";

  protected:
    ConnectorResult submit_to_venue(const Order&, const std::string&, std::string& venue_order_id) override {
        ++submit_calls;
        venue_order_id = next_venue_order_id;
        return submit_result;
    }

    ConnectorResult cancel_at_venue(const VenueOrderEntry&) override {
        ++cancel_calls;
        return cancel_result;
    }

    ConnectorResult replace_at_venue(const VenueOrderEntry&, const Order&, std::string& new_venue_order_id) override {
        ++replace_calls;
        new_venue_order_id = next_venue_order_id;
        return replace_result;
    }

    ConnectorResult query_at_venue(const VenueOrderEntry&, FillUpdate&) override {
        return ConnectorResult::OK;
    }

    ConnectorResult cancel_all_at_venue(const char*) override { return ConnectorResult::OK; }
};

Order make_order(uint64_t id) {
    Order o{};
    o.client_order_id = id;
    o.exchange = Exchange::BINANCE;
    o.side = Side::BID;
    o.type = OrderType::LIMIT;
    o.tif = TimeInForce::GTC;
    o.price = 100.0;
    o.quantity = 0.1;
    std::strncpy(o.symbol, "BTCUSDT", sizeof(o.symbol) - 1);
    o.symbol[sizeof(o.symbol) - 1] = '\0';
    return o;
}

const char* journal_path() { return "/tmp/trt_idempotency_BINANCE.log"; }

} // namespace

TEST(IdempotencyRecoveryTest, DuplicateSubmitAckRecoveredWithoutVenueCall) {
    std::remove(journal_path());
    {
        FILE* f = std::fopen(journal_path(), "w");
        ASSERT_NE(f, nullptr);
        std::fprintf(f, "1 0 100 0 1 2 venue-acked\n");
        std::fclose(f);
    }

    FakeConnector c(Exchange::BINANCE);
    const ConnectorResult result = c.submit_order(make_order(100));

    EXPECT_EQ(result, ConnectorResult::OK);
    EXPECT_EQ(c.submit_calls, 0);
    const VenueOrderEntry* mapped = c.order_map().get(100);
    ASSERT_NE(mapped, nullptr);
    EXPECT_STREQ(mapped->venue_order_id, "venue-acked");
}

TEST(IdempotencyRecoveryTest, RetryStormPersistsAndRecoversDeterministically) {
    std::remove(journal_path());

    FakeConnector c(Exchange::BINANCE);
    c.submit_result = ConnectorResult::ERROR_REST_FAILURE;

    const ConnectorResult first = c.submit_order(make_order(77));
    EXPECT_EQ(first, ConnectorResult::ERROR_REST_FAILURE);
    EXPECT_EQ(c.submit_calls, 3);

    c.submit_result = ConnectorResult::OK;
    c.next_venue_order_id = "venue-retry";

    const ConnectorResult second = c.submit_order(make_order(77));
    EXPECT_EQ(second, ConnectorResult::OK);
    EXPECT_EQ(c.submit_calls, 4);

    const VenueOrderEntry* mapped = c.order_map().get(77);
    ASSERT_NE(mapped, nullptr);
    EXPECT_STREQ(mapped->venue_order_id, "venue-retry");
}

TEST(IdempotencyRecoveryTest, CancelReplaceRaceReturnsInvalidOrderDeterministically) {
    std::remove(journal_path());

    FakeConnector c(Exchange::BINANCE);
    EXPECT_EQ(c.submit_order(make_order(11)), ConnectorResult::OK);

    Order replacement = make_order(12);
    c.next_venue_order_id = "venue-replaced";
    EXPECT_EQ(c.replace_order(11, replacement), ConnectorResult::OK);

    EXPECT_EQ(c.cancel_order(11), ConnectorResult::ERROR_INVALID_ORDER);
    EXPECT_EQ(c.cancel_calls, 0);
}

TEST(IdempotencyRecoveryTest, RecoveryGuardTriggersKillSwitchOnStormSignal) {
    KillSwitch kill_switch;
    RecoveryGuardConfig cfg;
    cfg.max_in_flight_ops = 2;
    cfg.max_duplicate_acks = 1;
    cfg.max_cancel_replace_races = 1;

    RecoveryGuard guard(cfg, kill_switch);
    EXPECT_TRUE(guard.check(1, 1, 0));
    EXPECT_FALSE(kill_switch.is_active());

    EXPECT_FALSE(guard.check(3, 1, 0));
    EXPECT_TRUE(kill_switch.is_active());
    EXPECT_EQ(kill_switch.get_reason(), KillReason::CIRCUIT_BREAKER);
}

} // namespace trading
