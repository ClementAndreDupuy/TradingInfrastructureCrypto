#include "core/feeds/binance/binance_feed_handler.hpp"
#include <gtest/gtest.h>
#include <string>

using namespace trading;

class BinanceFeedHandlerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        set_log_level(LogLevel::ERROR);
        handler_ = std::make_unique<BinanceFeedHandler>("BTCUSDT");

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
    }

    std::unique_ptr<BinanceFeedHandler> handler_;
    Snapshot last_snapshot_;
    std::vector<Delta> deltas_;
    std::string last_error_;
    int snapshot_count_ = 0;
};

TEST_F(BinanceFeedHandlerTest, HandlerCreation) {
    EXPECT_NE(handler_, nullptr);
    EXPECT_FALSE(handler_->is_running());
    EXPECT_EQ(handler_->get_sequence(), 0);
}

TEST_F(BinanceFeedHandlerTest, HandlerCreationWithApiKey) {
    auto handler_with_key =
        std::make_unique<BinanceFeedHandler>("BTCUSDT", "test_api_key", "test_api_secret");
    EXPECT_NE(handler_with_key, nullptr);
    EXPECT_FALSE(handler_with_key->is_running());
}

TEST_F(BinanceFeedHandlerTest, EnvVariableLoading) {
    std::string key = BinanceFeedHandler::get_api_key_from_env();
    std::string secret = BinanceFeedHandler::get_api_secret_from_env();
}

TEST_F(BinanceFeedHandlerTest, StartHandler) {
    EXPECT_EQ(handler_->start(), Result::SUCCESS);
    EXPECT_TRUE(handler_->is_running());
    EXPECT_GT(handler_->get_sequence(), 0);
}

TEST_F(BinanceFeedHandlerTest, ProcessValidDelta) {
    handler_->start();
    uint64_t initial_seq = handler_->get_sequence();
    deltas_.clear();

    std::string msg = R"({"e":"depthUpdate","s":"BTCUSDT","U":)" + std::to_string(initial_seq + 1) +
                      R"(,"u":)" + std::to_string(initial_seq + 1) +
                      R"(,"b":[["50000.00","1.5"]],"a":[]})";

    EXPECT_EQ(handler_->process_message(msg), Result::SUCCESS);
    EXPECT_EQ(handler_->get_sequence(), initial_seq + 1);
}

TEST_F(BinanceFeedHandlerTest, SequenceValidation) {
    handler_->start();
    uint64_t initial_seq = handler_->get_sequence();

    std::string msg = R"({"e":"depthUpdate","s":"BTCUSDT","U":)" +
                      std::to_string(initial_seq + 100) + R"(,"u":)" +
                      std::to_string(initial_seq + 100) + R"(,"b":[],"a":[]})";

    EXPECT_EQ(handler_->process_message(msg), Result::ERROR_SEQUENCE_GAP);
    EXPECT_FALSE(last_error_.empty());
}

TEST_F(BinanceFeedHandlerTest, IgnoreNonDepthMessages) {
    handler_->start();
    size_t delta_count_before = deltas_.size();

    std::string msg = R"({"e":"trade","s":"BTCUSDT","t":12345,"p":"50000.00","q":"1.5"})";

    EXPECT_EQ(handler_->process_message(msg), Result::SUCCESS);
    EXPECT_EQ(deltas_.size(), delta_count_before);
}

TEST_F(BinanceFeedHandlerTest, MissingSequenceFields) {
    handler_->start();

    std::string msg = R"({"e":"depthUpdate","s":"BTCUSDT","u":12345,"b":[],"a":[]})";

    EXPECT_EQ(handler_->process_message(msg), Result::ERROR_INVALID_SEQUENCE);
}

TEST_F(BinanceFeedHandlerTest, MalformedJsonIsIgnored) {
    EXPECT_EQ(handler_->process_message("{not-json"), Result::SUCCESS);
}

TEST_F(BinanceFeedHandlerTest, DuplicateSequenceIsRejected) {
    handler_->start();
    uint64_t initial_seq = handler_->get_sequence();

    std::string msg = R"({"e":"depthUpdate","s":"BTCUSDT","U":)" + std::to_string(initial_seq + 1) +
                      R"(,"u":)" + std::to_string(initial_seq + 1) +
                      R"(,"b":[["50000.00","1.5"]],"a":[]})";

    EXPECT_EQ(handler_->process_message(msg), Result::SUCCESS);
    EXPECT_EQ(handler_->process_message(msg), Result::ERROR_SEQUENCE_GAP);
}

TEST_F(BinanceFeedHandlerTest, ExtremePriceLevelParsesWithoutCrash) {
    handler_->start();
    uint64_t initial_seq = handler_->get_sequence();

    std::string msg = R"({"e":"depthUpdate","s":"BTCUSDT","U":)" + std::to_string(initial_seq + 1) +
                      R"(,"u":)" + std::to_string(initial_seq + 1) +
                      R"(,"b":[["1000000000.00","0.1"]],"a":[["0.00000001","0.1"]]})";

    EXPECT_EQ(handler_->process_message(msg), Result::SUCCESS);
    EXPECT_EQ(handler_->get_sequence(), initial_seq + 1);
}

TEST_F(BinanceFeedHandlerTest, StopHandler) {
    handler_->start();
    EXPECT_TRUE(handler_->is_running());

    handler_->stop();
    EXPECT_FALSE(handler_->is_running());

    EXPECT_EQ(handler_->start(), Result::SUCCESS);
    EXPECT_TRUE(handler_->is_running());
}

TEST_F(BinanceFeedHandlerTest, BinanceSequenceRule) {
    handler_->start();
    uint64_t last_id = handler_->get_sequence();

    std::string msg1 = R"({"e":"depthUpdate","s":"BTCUSDT","U":)" + std::to_string(last_id + 1) +
                       R"(,"u":)" + std::to_string(last_id + 1) + R"(,"b":[],"a":[]})";
    EXPECT_EQ(handler_->process_message(msg1), Result::SUCCESS);

    last_id = handler_->get_sequence();

    std::string msg2 = R"({"e":"depthUpdate","s":"BTCUSDT","U":)" + std::to_string(last_id) +
                       R"(,"u":)" + std::to_string(last_id + 2) + R"(,"b":[],"a":[]})";
    EXPECT_EQ(handler_->process_message(msg2), Result::SUCCESS);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
