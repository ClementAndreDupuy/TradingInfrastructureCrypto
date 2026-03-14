#include "core/feeds/okx/okx_feed_handler.hpp"
#include <gtest/gtest.h>

using namespace trading;

class OkxFeedHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        Logger::min_level() = LogLevel::ERROR;
        handler_ = std::make_unique<OkxFeedHandler>("BTC-USDT");

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

    std::unique_ptr<OkxFeedHandler> handler_;
    Snapshot last_snapshot_;
    std::vector<Delta> deltas_;
    std::string last_error_;
    int snapshot_count_ = 0;
};

TEST_F(OkxFeedHandlerTest, HandlerCreation) {
    EXPECT_NE(handler_, nullptr);
    EXPECT_FALSE(handler_->is_running());
    EXPECT_EQ(handler_->get_sequence(), 0u);
}

TEST_F(OkxFeedHandlerTest, IgnoresNonBookMessages) {
    std::string msg = R"({"event":"subscribe","arg":{"channel":"trades","instId":"BTC-USDT"}})";
    EXPECT_EQ(handler_->process_message(msg), Result::SUCCESS);
    EXPECT_TRUE(deltas_.empty());
}

TEST_F(OkxFeedHandlerTest, BuffersBookDeltasBeforeStreaming) {
    std::string msg =
        R"({"arg":{"channel":"books","instId":"BTC-USDT"},"data":[{"seqId":"101","prevSeqId":"100","bids":[["50000","1.0","0","1"]],"asks":[["50001","1.2","0","1"]]}]})";

    EXPECT_EQ(handler_->process_message(msg), Result::SUCCESS);
    EXPECT_TRUE(deltas_.empty());
    EXPECT_TRUE(last_error_.empty());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
