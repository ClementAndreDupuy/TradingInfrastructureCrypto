#include "core/feeds/coinbase/coinbase_feed_handler.hpp"
#include <algorithm>
#include <gtest/gtest.h>
#include <cstdlib>

using namespace trading;

class CoinbaseFeedHandlerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        set_log_level(LogLevel::ERROR);
        unsetenv("COINBASE_API_KEY");
        unsetenv("COINBASE_API_SECRET");
        unsetenv("LIVE_COINBASE_API_KEY");
        unsetenv("LIVE_COINBASE_API_SECRET");
        unsetenv("SHADOW_COINBASE_API_KEY");
        unsetenv("SHADOW_COINBASE_API_SECRET");
        handler_ = std::make_unique<CoinbaseFeedHandler>("BTC-USD", "wss://advanced-trade-ws.coinbase.com",
                                                           "https://api.coinbase.com");

        handler_->set_snapshot_callback([this](const Snapshot& snapshot) {
            last_snapshot_ = snapshot;
            snapshot_count_++;
        });

        handler_->set_delta_callback([this](const Delta& delta) { deltas_.push_back(delta); });

        handler_->set_error_callback([this](const std::string& error) { last_error_ = error; });
    }

    void TearDown() override {
        if (handler_)
            handler_->stop();
        unsetenv("COINBASE_API_KEY");
        unsetenv("COINBASE_API_SECRET");
        unsetenv("LIVE_COINBASE_API_KEY");
        unsetenv("LIVE_COINBASE_API_SECRET");
        unsetenv("SHADOW_COINBASE_API_KEY");
        unsetenv("SHADOW_COINBASE_API_SECRET");
    }

    std::unique_ptr<CoinbaseFeedHandler> handler_;
    Snapshot last_snapshot_;
    std::vector<Delta> deltas_;
    std::string last_error_;
    int snapshot_count_ = 0;
};

TEST_F(CoinbaseFeedHandlerTest, HandlerCreation) {
    EXPECT_NE(handler_, nullptr);
    EXPECT_FALSE(handler_->is_running());
    EXPECT_EQ(handler_->get_sequence(), 0u);
    EXPECT_EQ(handler_->tick_size(), 0.0);
}

TEST_F(CoinbaseFeedHandlerTest, SubscriptionMessagesIncludeHeartbeatsAndLevel2) {
    const auto messages = handler_->build_subscription_messages();
    ASSERT_EQ(messages.size(), 2u);

    const auto heartbeat = nlohmann::json::parse(messages[0]);
    const auto level2 = nlohmann::json::parse(messages[1]);

    EXPECT_EQ(heartbeat.at("channel").get<std::string>(), "heartbeats");
    EXPECT_EQ(level2.at("channel").get<std::string>(), "level2");
    EXPECT_FALSE(heartbeat.contains("product_ids"));
    ASSERT_TRUE(level2.contains("product_ids"));
    EXPECT_EQ(level2.at("product_ids").size(), 1u);
    EXPECT_FALSE(heartbeat.contains("jwt"));
    EXPECT_FALSE(level2.contains("jwt"));
}

TEST_F(CoinbaseFeedHandlerTest, SubscriptionMessagesAttachJwtWhenCredentialsAvailable) {
    setenv("COINBASE_API_KEY", "organizations/test/apiKeys/test-key", 1);
    setenv("COINBASE_API_SECRET",
           "-----BEGIN EC PRIVATE KEY-----\n"
           "MHcCAQEEIDW7g+mp4sHttM8M7e95rrNTcYWc4V85qmHLn6auutnFoAoGCCqGSM49\n"
           "AwEHoUQDQgAE5DsgL4nmY0tP1Z+P3IhGLFIB7S0KljPz3/fyJAT5aQ6tW5JNo16t\n"
           "Mny9q6iIhS0drwWzDz5aUSsfx8bVsgcuyo==\n"
           "-----END EC PRIVATE KEY-----\n",
           1);

    const auto direct_jwt = CoinbaseFeedHandler::generate_jwt(
        CoinbaseFeedHandler::coinbase_api_key_from_env(),
        CoinbaseFeedHandler::coinbase_api_secret_from_env());
    const auto messages = handler_->build_subscription_messages();
    ASSERT_EQ(messages.size(), 2u);

    for (const auto& message : messages) {
        const auto json = nlohmann::json::parse(message);
        if (direct_jwt.empty()) {
            EXPECT_FALSE(json.contains("jwt"));
        } else {
            ASSERT_TRUE(json.contains("jwt"));
            const std::string jwt = json.at("jwt").get<std::string>();
            EXPECT_FALSE(jwt.empty());
            EXPECT_EQ(std::count(jwt.begin(), jwt.end(), '.'), 2);
        }
    }
}

TEST_F(CoinbaseFeedHandlerTest, ProcessSnapshotThenUpdate) {
    std::string snapshot =
        R"({"channel":"l2_data","timestamp":"2026-03-19T12:00:00.123456789Z","sequence_num":100,"events":[{"type":"snapshot","updates":[{"side":"bid","price_level":"50000.0","new_quantity":"1.2"},{"side":"offer","price_level":"50001.0","new_quantity":"0.8"}]}]})";

    EXPECT_EQ(handler_->process_message(snapshot), Result::SUCCESS);
    EXPECT_EQ(snapshot_count_, 1);
    EXPECT_EQ(last_snapshot_.exchange, Exchange::COINBASE);
    EXPECT_EQ(handler_->get_sequence(), 100u);
    EXPECT_GT(last_snapshot_.timestamp_exchange_ns, 0);

    std::string update =
        R"({"channel":"l2_data","timestamp":"2026-03-19T12:00:01.123456789Z","sequence_num":101,"events":[{"type":"update","updates":[{"side":"bid","price_level":"50000.5","new_quantity":"2.0"},{"side":"offer","price_level":"50001.5","new_quantity":"1.1"}]}]})";

    EXPECT_EQ(handler_->process_message(update), Result::SUCCESS);
    ASSERT_EQ(deltas_.size(), 2u);
    EXPECT_EQ(deltas_[0].side, Side::BID);
    EXPECT_EQ(deltas_[1].side, Side::ASK);
    EXPECT_EQ(handler_->get_sequence(), 101u);
    EXPECT_GT(deltas_[0].timestamp_exchange_ns, 0);
}

TEST_F(CoinbaseFeedHandlerTest, HeartbeatMessagesAreAccepted) {
    const std::string heartbeat =
        R"({"channel":"heartbeats","timestamp":"2026-03-19T12:00:02.000000000Z","events":[{"current_time":"2026-03-19T12:00:02.000000000Z","heartbeat_counter":"10"}]})";
    EXPECT_EQ(handler_->process_message(heartbeat), Result::SUCCESS);
    EXPECT_TRUE(last_error_.empty());
}

TEST_F(CoinbaseFeedHandlerTest, IgnoresLegacyLevel2Channel) {
    std::string snapshot =
        R"({"channel":"level2","sequence_num":100,"events":[{"type":"snapshot","updates":[{"side":"bid","price_level":"50000.0","new_quantity":"1.2"},{"side":"offer","price_level":"50001.0","new_quantity":"0.8"}]}]})";

    EXPECT_EQ(handler_->process_message(snapshot), Result::SUCCESS);
    EXPECT_EQ(snapshot_count_, 0);
    EXPECT_EQ(handler_->get_sequence(), 0u);
}

TEST_F(CoinbaseFeedHandlerTest, MalformedJsonIsIgnored) {
    EXPECT_EQ(handler_->process_message("{bad-json"), Result::SUCCESS);
}

TEST_F(CoinbaseFeedHandlerTest, DuplicateSequenceRejectedAfterSnapshot) {
    std::string snapshot =
        R"({"channel":"l2_data","sequence_num":100,"events":[{"type":"snapshot","updates":[{"side":"bid","price_level":"50000.0","new_quantity":"1.2"},{"side":"offer","price_level":"50001.0","new_quantity":"0.8"}]}]})";
    EXPECT_EQ(handler_->process_message(snapshot), Result::SUCCESS);

    std::string update =
        R"({"channel":"l2_data","sequence_num":101,"events":[{"type":"update","updates":[{"side":"bid","price_level":"50000.5","new_quantity":"2.0"}]}]})";
    EXPECT_EQ(handler_->process_message(update), Result::SUCCESS);
    EXPECT_EQ(handler_->process_message(update), Result::ERROR_SEQUENCE_GAP);
}

TEST_F(CoinbaseFeedHandlerTest, ExtremePriceLevelsAreHandled) {
    std::string snapshot =
        R"({"channel":"l2_data","sequence_num":200,"events":[{"type":"snapshot","updates":[{"side":"bid","price_level":"1000000000.0","new_quantity":"1.2"},{"side":"offer","price_level":"0.00000001","new_quantity":"0.8"}]}]})";
    EXPECT_EQ(handler_->process_message(snapshot), Result::SUCCESS);
    EXPECT_EQ(handler_->get_sequence(), 200u);
}

TEST_F(CoinbaseFeedHandlerTest, SequenceGapTriggersError) {
    std::string snapshot =
        R"({"channel":"l2_data","sequence_num":100,"events":[{"type":"snapshot","updates":[{"side":"bid","price_level":"50000.0","new_quantity":"1.2"},{"side":"offer","price_level":"50001.0","new_quantity":"0.8"}]}]})";
    EXPECT_EQ(handler_->process_message(snapshot), Result::SUCCESS);

    std::string gapped =
        R"({"channel":"l2_data","sequence_num":103,"events":[{"type":"update","updates":[{"side":"bid","price_level":"50000.5","new_quantity":"2.0"}]}]})";

    EXPECT_EQ(handler_->process_message(gapped), Result::ERROR_SEQUENCE_GAP);
    EXPECT_FALSE(last_error_.empty());
}

TEST_F(CoinbaseFeedHandlerTest, AuthRejectSurfacesSpecificError) {
    const std::string error =
        R"({"type":"error","message":"jwt expired for subscription"})";
    EXPECT_EQ(handler_->process_message(error), Result::ERROR_CONNECTION_LOST);
    EXPECT_NE(last_error_.find("authentication rejected"), std::string::npos);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
