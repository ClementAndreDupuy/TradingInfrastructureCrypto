#include "core/feeds/kraken/kraken_feed_handler.hpp"
#include <gtest/gtest.h>
#include <string>

using namespace trading;

class KrakenFeedHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        Logger::min_level() = LogLevel::ERROR;
        handler_ = std::make_unique<KrakenFeedHandler>("XBTUSD");

        handler_->set_snapshot_callback([this](const Snapshot& snapshot) {
            last_snapshot_ = snapshot;
            snapshot_count_++;
        });

        handler_->set_delta_callback([this](const Delta& delta) {
            deltas_.push_back(delta);
        });

        handler_->set_error_callback([this](const std::string& error) {
            last_error_ = error;
        });
    }

    void TearDown() override {
        if (handler_) handler_->stop();
    }

    std::unique_ptr<KrakenFeedHandler> handler_;
    Snapshot last_snapshot_;
    std::vector<Delta> deltas_;
    std::string last_error_;
    int snapshot_count_ = 0;
};

TEST_F(KrakenFeedHandlerTest, HandlerCreation) {
    EXPECT_NE(handler_, nullptr);
    EXPECT_FALSE(handler_->is_running());
    EXPECT_EQ(handler_->get_sequence(), 0);
}

TEST_F(KrakenFeedHandlerTest, HandlerCreationWithApiKey) {
    auto h = std::make_unique<KrakenFeedHandler>("XBTUSD", "test_api_key", "test_api_secret");
    EXPECT_NE(h, nullptr);
    EXPECT_FALSE(h->is_running());
}

TEST_F(KrakenFeedHandlerTest, EnvVariableLoading) {
    std::string key = KrakenFeedHandler::get_api_key_from_env();
    std::string secret = KrakenFeedHandler::get_api_secret_from_env();
    // No assertion: env vars may or may not be set.
}

TEST_F(KrakenFeedHandlerTest, StartHandler) {
    EXPECT_EQ(handler_->start(), Result::SUCCESS);
    EXPECT_TRUE(handler_->is_running());
    // Kraken REST has no sequence number; last_seq_ stays at 0 after snapshot.
    EXPECT_EQ(handler_->get_sequence(), 0);
    EXPECT_EQ(snapshot_count_, 1);
    EXPECT_EQ(last_snapshot_.exchange, Exchange::KRAKEN);
    EXPECT_FALSE(last_snapshot_.bids.empty());
    EXPECT_FALSE(last_snapshot_.asks.empty());
}

TEST_F(KrakenFeedHandlerTest, ProcessValidUpdate) {
    handler_->start();
    deltas_.clear();

    // seq=1 is valid: last_seq_=0, 0+1=1.
    std::string msg =
        R"({"channel":"book","type":"update","seq":1,"data":[{"symbol":"XBTUSD",)"
        R"("bids":[{"price":50000.0,"qty":1.5}],"asks":[],"checksum":0,"timestamp":"2024-01-01T00:00:00Z"}]})";

    EXPECT_EQ(handler_->process_message(msg), Result::SUCCESS);
    EXPECT_EQ(handler_->get_sequence(), 1);
    ASSERT_EQ(deltas_.size(), 1u);
    EXPECT_EQ(deltas_[0].side, Side::BID);
    EXPECT_DOUBLE_EQ(deltas_[0].price, 50000.0);
    EXPECT_DOUBLE_EQ(deltas_[0].size, 1.5);
    EXPECT_EQ(deltas_[0].sequence, 1u);
}

TEST_F(KrakenFeedHandlerTest, ProcessBidAndAskUpdate) {
    handler_->start();
    deltas_.clear();

    std::string msg =
        R"({"channel":"book","type":"update","seq":1,"data":[{"symbol":"XBTUSD",)"
        R"("bids":[{"price":50000.0,"qty":1.5}],"asks":[{"price":50001.0,"qty":2.0}],)"
        R"("checksum":0,"timestamp":"2024-01-01T00:00:00Z"}]})";

    EXPECT_EQ(handler_->process_message(msg), Result::SUCCESS);
    ASSERT_EQ(deltas_.size(), 2u);
    EXPECT_EQ(deltas_[0].side, Side::BID);
    EXPECT_EQ(deltas_[1].side, Side::ASK);
    EXPECT_DOUBLE_EQ(deltas_[1].price, 50001.0);
}

TEST_F(KrakenFeedHandlerTest, SequenceValidation) {
    handler_->start();

    // seq=100 with last_seq_=0 is a gap.
    std::string msg =
        R"({"channel":"book","type":"update","seq":100,"data":[{"symbol":"XBTUSD",)"
        R"("bids":[],"asks":[],"checksum":0}]})";

    EXPECT_EQ(handler_->process_message(msg), Result::ERROR_SEQUENCE_GAP);
    EXPECT_FALSE(last_error_.empty());
}

TEST_F(KrakenFeedHandlerTest, StrictSequenceRule) {
    handler_->start();

    // seq=1 accepted (last_seq_=0).
    std::string msg1 =
        R"({"channel":"book","type":"update","seq":1,"data":[{"symbol":"XBTUSD",)"
        R"("bids":[{"price":50000.0,"qty":1.0}],"asks":[],"checksum":0}]})";
    EXPECT_EQ(handler_->process_message(msg1), Result::SUCCESS);
    EXPECT_EQ(handler_->get_sequence(), 1u);

    // seq=3 rejected: last_seq_=1, expected 2. Kraken is strictly +1, no window.
    std::string msg2 =
        R"({"channel":"book","type":"update","seq":3,"data":[{"symbol":"XBTUSD",)"
        R"("bids":[],"asks":[],"checksum":0}]})";
    EXPECT_EQ(handler_->process_message(msg2), Result::ERROR_SEQUENCE_GAP);

    // seq=2 would now also be rejected because trigger_resnapshot cleared state.
}

TEST_F(KrakenFeedHandlerTest, IgnoreNonBookMessages) {
    handler_->start();
    size_t before = deltas_.size();

    // Heartbeat has no "channel":"book".
    std::string msg = R"({"channel":"heartbeat","timestamp":"2024-01-01T00:00:00Z"})";
    EXPECT_EQ(handler_->process_message(msg), Result::SUCCESS);
    EXPECT_EQ(deltas_.size(), before);
}

TEST_F(KrakenFeedHandlerTest, MissingSeqField) {
    handler_->start();

    std::string msg =
        R"({"channel":"book","type":"update","data":[{"symbol":"XBTUSD","bids":[],"asks":[]}]})";

    EXPECT_EQ(handler_->process_message(msg), Result::ERROR_INVALID_SEQUENCE);
}

TEST_F(KrakenFeedHandlerTest, StopAndRestart) {
    handler_->start();
    EXPECT_TRUE(handler_->is_running());

    handler_->stop();
    EXPECT_FALSE(handler_->is_running());

    EXPECT_EQ(handler_->start(), Result::SUCCESS);
    EXPECT_TRUE(handler_->is_running());
}

TEST_F(KrakenFeedHandlerTest, ExchangeTaggedCorrectly) {
    handler_->start();
    EXPECT_EQ(last_snapshot_.exchange, Exchange::KRAKEN);
    EXPECT_EQ(std::string(exchange_to_string(Exchange::KRAKEN)), "KRAKEN");
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
