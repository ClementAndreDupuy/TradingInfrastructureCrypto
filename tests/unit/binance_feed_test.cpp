#define private public
#include "core/feeds/binance/binance_feed_handler.hpp"
#undef private
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

    BinanceFeedHandler::BufferedDelta make_delta(uint64_t first_update_id, uint64_t last_update_id,
                                                 double bid_price = 50000.0,
                                                 double ask_price = 50001.0) {
        BinanceFeedHandler::BufferedDelta delta;
        delta.first_update_id = first_update_id;
        delta.last_update_id = last_update_id;
        delta.timestamp_exchange_ns = 1700000000000000000LL + static_cast<int64_t>(last_update_id);
        delta.bids.push_back({bid_price, 1.5});
        delta.asks.push_back({ask_price, 0.8});
        return delta;
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
    EXPECT_EQ(handler_->get_sequence(), 0u);
    EXPECT_EQ(handler_->tick_size(), 0.0);
}

TEST_F(BinanceFeedHandlerTest, MissingSequenceFields) {
    std::string msg = R"({"e":"depthUpdate","s":"BTCUSDT","u":12345,"b":[],"a":[]})";
    EXPECT_EQ(handler_->process_message(msg), Result::ERROR_INVALID_SEQUENCE);
}

TEST_F(BinanceFeedHandlerTest, MalformedJsonIsIgnored) {
    EXPECT_EQ(handler_->process_message("{not-json"), Result::SUCCESS);
}

TEST_F(BinanceFeedHandlerTest, IgnoreNonDepthMessages) {
    std::string msg = R"({"e":"trade","s":"BTCUSDT","t":12345,"p":"50000.00","q":"1.5"})";
    EXPECT_EQ(handler_->process_message(msg), Result::SUCCESS);
    EXPECT_TRUE(deltas_.empty());
}

TEST_F(BinanceFeedHandlerTest, BuffersParsedDeltasWhileSynchronizing) {
    std::string msg =
        R"({"e":"depthUpdate","E":1700000000000,"s":"BTCUSDT","U":101,"u":102,"b":[["50000.00","1.5"]],"a":[["50001.00","0.8"]]})";

    EXPECT_EQ(handler_->process_message(msg), Result::SUCCESS);
    ASSERT_EQ(handler_->delta_buffer_.size(), 1u);
    EXPECT_EQ(handler_->delta_buffer_[0].first_update_id, 101u);
    EXPECT_EQ(handler_->delta_buffer_[0].last_update_id, 102u);
    EXPECT_EQ(handler_->delta_buffer_[0].timestamp_exchange_ns, 1700000000000000000LL);
    auto stats = handler_->sync_stats();
    EXPECT_EQ(stats.buffer_high_water_mark, 1u);
}

TEST_F(BinanceFeedHandlerTest, ApplyBufferedDeltasSkipsStaleAndBridgesSnapshot) {
    handler_->delta_buffer_.push_back(make_delta(95, 99));
    handler_->delta_buffer_.push_back(make_delta(100, 102));
    handler_->delta_buffer_.push_back(make_delta(103, 104, 50000.5, 50001.5));
    handler_->last_sequence_.store(99, std::memory_order_release);
    handler_->state_.store(BinanceFeedHandler::State::BUFFERING, std::memory_order_release);

    ASSERT_EQ(handler_->apply_buffered_deltas(99), Result::SUCCESS);
    EXPECT_EQ(handler_->get_sequence(), 104u);
    EXPECT_EQ(deltas_.size(), 4u);
    auto stats = handler_->sync_stats();
    EXPECT_EQ(stats.buffered_applied, 2u);
    EXPECT_EQ(stats.buffered_skipped, 1u);
    EXPECT_EQ(stats.buffer_high_water_mark, 0u);
}


TEST_F(BinanceFeedHandlerTest, AllBufferedDeltasOlderThanSnapshotAreIgnored) {
    handler_->delta_buffer_.push_back(make_delta(95, 99));
    handler_->delta_buffer_.push_back(make_delta(98, 99, 50000.5, 50001.5));
    handler_->last_sequence_.store(100, std::memory_order_release);

    ASSERT_EQ(handler_->apply_buffered_deltas(100), Result::SUCCESS);
    EXPECT_TRUE(handler_->delta_buffer_.empty());
    EXPECT_TRUE(deltas_.empty());
    auto stats = handler_->sync_stats();
    EXPECT_EQ(stats.buffered_skipped, 2u);
}

TEST_F(BinanceFeedHandlerTest, MissingBridgeDeltaTriggersResync) {
    handler_->delta_buffer_.push_back(make_delta(105, 106));
    handler_->last_sequence_.store(99, std::memory_order_release);

    EXPECT_EQ(handler_->apply_buffered_deltas(99), Result::ERROR_SEQUENCE_GAP);
    auto stats = handler_->sync_stats();
    EXPECT_EQ(stats.resync_count, 1u);
    EXPECT_EQ(stats.last_resync_reason, "snapshot_handoff_gap");
    EXPECT_NE(last_error_.find("snapshot_handoff_gap"), std::string::npos);
}


TEST_F(BinanceFeedHandlerTest, StaleStreamingDeltaIsIgnoredPerBinanceContract) {
    handler_->state_.store(BinanceFeedHandler::State::STREAMING, std::memory_order_release);
    handler_->last_sequence_.store(105, std::memory_order_release);

    std::string msg = R"({"e":"depthUpdate","s":"BTCUSDT","U":100,"u":104,"b":[["50000.00","1.0"]],"a":[]})";
    EXPECT_EQ(handler_->process_message(msg), Result::SUCCESS);
    EXPECT_TRUE(deltas_.empty());
    EXPECT_EQ(handler_->get_sequence(), 105u);
    EXPECT_EQ(handler_->sync_stats().resync_count, 0u);
}

TEST_F(BinanceFeedHandlerTest, StreamingSequenceGapTriggersResync) {
    handler_->state_.store(BinanceFeedHandler::State::STREAMING, std::memory_order_release);
    handler_->last_sequence_.store(100, std::memory_order_release);

    std::string msg = R"({"e":"depthUpdate","s":"BTCUSDT","U":105,"u":105,"b":[],"a":[]})";
    EXPECT_EQ(handler_->process_message(msg), Result::ERROR_SEQUENCE_GAP);

    auto stats = handler_->sync_stats();
    EXPECT_EQ(stats.resync_count, 1u);
    EXPECT_EQ(stats.last_resync_reason, "sequence_gap");
    EXPECT_TRUE(handler_->reconnect_requested_.load(std::memory_order_acquire));
}

TEST_F(BinanceFeedHandlerTest, ParseFailureReturnsBookCorrupted) {
    std::string msg = R"({"e":"depthUpdate","s":"BTCUSDT","U":101,"u":101,"b":[["bad","1.0"]],"a":[]})";
    EXPECT_EQ(handler_->process_message(msg), Result::ERROR_BOOK_CORRUPTED);
}

TEST_F(BinanceFeedHandlerTest, BufferOverflowTriggersResync) {
    handler_->delta_buffer_.resize(BinanceFeedHandler::MAX_BUFFER_SIZE);
    std::string msg = R"({"e":"depthUpdate","s":"BTCUSDT","U":101,"u":101,"b":[],"a":[]})";

    EXPECT_EQ(handler_->process_message(msg), Result::ERROR_CONNECTION_LOST);
    auto stats = handler_->sync_stats();
    EXPECT_EQ(stats.resync_count, 1u);
    EXPECT_EQ(stats.last_resync_reason, "buffer_overflow");
    EXPECT_TRUE(handler_->delta_buffer_.empty());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
