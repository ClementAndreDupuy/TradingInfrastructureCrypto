#include "core/feeds/coinbase/coinbase_feed_handler.hpp"
#include <gtest/gtest.h>

using namespace trading;

class CoinbaseFeedHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        set_log_level(LogLevel::ERROR);
        handler_ = std::make_unique<CoinbaseFeedHandler>("BTC-USD");

        handler_->set_snapshot_callback([this](const Snapshot& s) {
            last_snapshot_ = s;
            snapshot_count_++;
        });

        handler_->set_delta_callback([this](const Delta& d) {
            deltas_.push_back(d);
        });

        handler_->set_error_callback([this](const std::string& e) {
            last_error_ = e;
        });
    }

    void TearDown() override {
        if (handler_) handler_->stop();
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
}

TEST_F(CoinbaseFeedHandlerTest, ProcessSnapshotThenUpdate) {
    std::string snapshot =
        R"({"channel":"l2_data","sequence_num":100,"events":[{"type":"snapshot","updates":[{"side":"bid","price_level":"50000.0","new_quantity":"1.2"},{"side":"offer","price_level":"50001.0","new_quantity":"0.8"}]}]})";

    EXPECT_EQ(handler_->process_message(snapshot), Result::SUCCESS);
    EXPECT_EQ(snapshot_count_, 1);
    EXPECT_EQ(last_snapshot_.exchange, Exchange::COINBASE);
    EXPECT_EQ(handler_->get_sequence(), 100u);

    std::string update =
        R"({"channel":"l2_data","sequence_num":101,"events":[{"type":"update","updates":[{"side":"bid","price_level":"50000.5","new_quantity":"2.0"},{"side":"offer","price_level":"50001.5","new_quantity":"1.1"}]}]})";

    EXPECT_EQ(handler_->process_message(update), Result::SUCCESS);
    ASSERT_EQ(deltas_.size(), 2u);
    EXPECT_EQ(deltas_[0].side, Side::BID);
    EXPECT_EQ(deltas_[1].side, Side::ASK);
    EXPECT_EQ(handler_->get_sequence(), 101u);
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
        R"({"channel":"l2_data","sequence_num":100,"events":[{"type":"snapshot","updates":[{"side":"bid","price_level":"50000.0","new_quantity":"1.2"}]}]})";
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

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
