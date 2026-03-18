#include "core/common/symbol_mapper.hpp"
#include "core/feeds/okx/okx_feed_handler.hpp"
#include <gtest/gtest.h>
#include <stdexcept>

using namespace trading;

class OkxFeedHandlerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        set_log_level(LogLevel::ERROR);
        handler_ = std::make_unique<OkxFeedHandler>("BTC-USDT");

        handler_->set_snapshot_callback([this](const Snapshot& s) {
            last_snapshot_ = s;
            snapshot_count_++;
        });

        handler_->set_delta_callback([this](const Delta& d) { deltas_.push_back(d); });

        handler_->set_error_callback([this](const std::string& e) { last_error_ = e; });
    }

    void TearDown() override {
        if (handler_)
            handler_->stop();
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

TEST_F(OkxFeedHandlerTest, MalformedJsonIsIgnored) {
    EXPECT_EQ(handler_->process_message("{oops"), Result::SUCCESS);
}

TEST_F(OkxFeedHandlerTest, SequenceGapRejectedInStreamingState) {
    handler_->set_streaming_state_for_test(100);
    handler_->seed_book_state_for_test({PriceLevel{50000.0, 1.0}}, {PriceLevel{50001.0, 1.0}});

    // prevSeqId=103 != last_sequence_=100: updates 101-103 were never received.
    std::string msg =
        R"({"arg":{"channel":"books","instId":"BTC-USDT"},"data":[{"seqId":"104","prevSeqId":"103","bids":[["50000","1.0","0","1"]],"asks":[["50001","1.0","0","1"]]}]})";
    EXPECT_EQ(handler_->process_message(msg), Result::ERROR_SEQUENCE_GAP);
}

TEST_F(OkxFeedHandlerTest, StaleUpdateRejectedInStreamingState) {
    handler_->set_streaming_state_for_test(100);
    handler_->seed_book_state_for_test({PriceLevel{50000.0, 1.0}}, {PriceLevel{50001.0, 1.0}});

    // prevSeqId=99 != last_sequence_=100: update is behind our current position.
    std::string msg =
        R"({"arg":{"channel":"books","instId":"BTC-USDT"},"data":[{"seqId":"100","prevSeqId":"99","bids":[["50000","1.0","0","1"]],"asks":[["50001","1.0","0","1"]]}]})";
    EXPECT_EQ(handler_->process_message(msg), Result::ERROR_SEQUENCE_GAP);
}

TEST_F(OkxFeedHandlerTest, BuffersBookDeltasBeforeStreaming) {
    std::string msg =
        R"({"arg":{"channel":"books","instId":"BTC-USDT"},"data":[{"seqId":"101","prevSeqId":"100","bids":[["50000","1.0","0","1"]],"asks":[["50001","1.2","0","1"]]}]})";

    EXPECT_EQ(handler_->process_message(msg), Result::SUCCESS);
    EXPECT_TRUE(deltas_.empty());
    EXPECT_TRUE(last_error_.empty());
}

TEST_F(OkxFeedHandlerTest, IgnoresMessagesForOtherInstrument) {
    handler_->set_streaming_state_for_test(100);
    handler_->seed_book_state_for_test({PriceLevel{50000.0, 1.0}}, {PriceLevel{50001.0, 1.0}});

    std::string msg =
        R"({"arg":{"channel":"books","instId":"ETH-USDT"},"data":[{"seqId":"101","prevSeqId":"100","bids":[["50000","1.0","0","1"]],"asks":[["50001","1.0","0","1"]]}]})";

    EXPECT_EQ(handler_->process_message(msg), Result::SUCCESS);
    EXPECT_TRUE(deltas_.empty());
}

TEST(OkxFeedHandlerSymbolTest, NormalizesCompactSymbolToOkxInstId) {
    OkxFeedHandler handler("BTCUSDT");
    handler.set_streaming_state_for_test(100);
    handler.seed_book_state_for_test({PriceLevel{50000.0, 1.0}}, {PriceLevel{50001.0, 1.0}});

    std::vector<Delta> deltas;
    handler.set_delta_callback([&deltas](const Delta& d) { deltas.push_back(d); });

    std::string msg =
        R"({"arg":{"channel":"books","instId":"BTC-USDT"},"data":[{"seqId":"101","prevSeqId":"100","bids":[["50000","1.0","0","1"]],"asks":[["50001","1.2","0","1"]]}]})";

    EXPECT_EQ(handler.process_message(msg), Result::SUCCESS);
    EXPECT_EQ(handler.get_sequence(), 101u);
    EXPECT_FALSE(deltas.empty());
}

TEST(SymbolMapperTest, MapsCompactSymbolAcrossExchanges) {
    const VenueSymbols symbols = SymbolMapper::map_all("BTCUSDT");
    EXPECT_EQ(symbols.binance, "BTCUSDT");
    EXPECT_EQ(symbols.okx, "BTC-USDT");
    EXPECT_EQ(symbols.coinbase, "BTC-USDT");
    EXPECT_EQ(symbols.kraken_ws, "BTC/USDT");
    EXPECT_EQ(symbols.kraken_rest, "BTC/USDT");
}

TEST(SymbolMapperTest, PreservesDelimitedInputAcrossExchanges) {
    const VenueSymbols symbols = SymbolMapper::map_all("eth-usdc");
    EXPECT_EQ(symbols.binance, "ETHUSDC");
    EXPECT_EQ(symbols.okx, "ETH-USDC");
    EXPECT_EQ(symbols.coinbase, "ETH-USDC");
    EXPECT_EQ(symbols.kraken_ws, "ETH/USDC");
}

TEST(SymbolMapperTest, ForExchangeReturnsCorrectVenueString) {
    const VenueSymbols symbols = SymbolMapper::map_all("BTCUSDT");
    EXPECT_EQ(symbols.for_exchange(Exchange::BINANCE), "BTCUSDT");
    EXPECT_EQ(symbols.for_exchange(Exchange::OKX), "BTC-USDT");
    EXPECT_EQ(symbols.for_exchange(Exchange::COINBASE), "BTC-USDT");
    EXPECT_EQ(symbols.for_exchange(Exchange::KRAKEN), "BTC/USDT");
}

TEST(SymbolMapperTest, MapForExchangeConvenienceWrapper) {
    EXPECT_EQ(SymbolMapper::map_for_exchange(Exchange::BINANCE, "BTC/USDT"), "BTCUSDT");
    EXPECT_EQ(SymbolMapper::map_for_exchange(Exchange::OKX, "BTCUSDT"), "BTC-USDT");
    EXPECT_EQ(SymbolMapper::map_for_exchange(Exchange::COINBASE, "BTC_USDT"), "BTC-USDT");
    EXPECT_EQ(SymbolMapper::map_for_exchange(Exchange::KRAKEN, "BTCUSDT"), "BTC/USDT");
}

TEST(SymbolMapperTest, EmptySymbolThrowsInvalidArgument) {
    EXPECT_THROW(SymbolMapper::map_all(""), std::invalid_argument);
    EXPECT_THROW(SymbolMapper::map_for_exchange(Exchange::BINANCE, ""), std::invalid_argument);
}
TEST_F(OkxFeedHandlerTest, ChecksumMismatchTriggersResnapshot) {
    handler_->set_streaming_state_for_test(100);
    handler_->seed_book_state_for_test({PriceLevel{50000.0, 1.0}}, {PriceLevel{50001.0, 1.0}});

    std::string msg =
        R"({"arg":{"channel":"books","instId":"BTC-USDT"},"data":[{"seqId":"101","prevSeqId":"100","checksum":123,"bids":[["50000","1.5","0","1"]],"asks":[["50001","1.1","0","1"]]}]})";

    EXPECT_EQ(handler_->process_message(msg), Result::ERROR_BOOK_CORRUPTED);
    EXPECT_FALSE(last_error_.empty());
}

// action:"snapshot" WS push initialises the book, fires the snapshot callback,
// and allows subsequent incremental updates to be processed normally.
TEST_F(OkxFeedHandlerTest, WsSnapshotInitializesBook) {
    std::string snap_msg = R"({"action":"snapshot","arg":{"channel":"books","instId":"BTC-USDT"},)"
                           R"("data":[{"bids":[["50000","1.0","0","1"],["49999","2.0","0","1"]],)"
                           R"("asks":[["50001","1.5","0","1"],["50002","0.5","0","1"]],)"
                           R"("ts":"1000","seqId":200}]})";

    EXPECT_EQ(handler_->process_message(snap_msg), Result::SUCCESS);
    EXPECT_EQ(handler_->get_sequence(), 200u);
    EXPECT_EQ(snapshot_count_, 1);
    EXPECT_EQ(last_snapshot_.sequence, 200u);
    ASSERT_EQ(last_snapshot_.bids.size(), 2u);
    ASSERT_EQ(last_snapshot_.asks.size(), 2u);
    EXPECT_DOUBLE_EQ(last_snapshot_.bids[0].price, 50000.0);
    EXPECT_DOUBLE_EQ(last_snapshot_.asks[0].price, 50001.0);
}

// After a WS snapshot the handler is in STREAMING state and processes deltas.
TEST_F(OkxFeedHandlerTest, DeltaProcessedAfterWsSnapshot) {
    std::string snap_msg =
        R"({"action":"snapshot","arg":{"channel":"books","instId":"BTC-USDT"},)"
        R"("data":[{"bids":[["50000","1.0","0","1"]],"asks":[["50001","1.5","0","1"]],)"
        R"("ts":"1000","seqId":100}]})";
    ASSERT_EQ(handler_->process_message(snap_msg), Result::SUCCESS);

    std::string update_msg = R"({"action":"update","arg":{"channel":"books","instId":"BTC-USDT"},)"
                             R"("data":[{"seqId":"101","prevSeqId":"100",)"
                             R"("bids":[["50000","2.0","0","1"]],"asks":[],"ts":"1001"}]})";

    EXPECT_EQ(handler_->process_message(update_msg), Result::SUCCESS);
    EXPECT_EQ(handler_->get_sequence(), 101u);
    ASSERT_FALSE(deltas_.empty());
    EXPECT_DOUBLE_EQ(deltas_[0].price, 50000.0);
    EXPECT_DOUBLE_EQ(deltas_[0].size, 2.0);
    EXPECT_EQ(deltas_[0].side, Side::BID);
}

// Updates that arrive while BUFFERING (before snapshot) are replayed after the
// WS snapshot is received; the handler ends up in STREAMING with the correct seq.
TEST_F(OkxFeedHandlerTest, BufferedUpdateAppliedAfterWsSnapshot) {
    // Send a delta before the snapshot (handler buffers it in DISCONNECTED state).
    std::string update_msg = R"({"action":"update","arg":{"channel":"books","instId":"BTC-USDT"},)"
                             R"("data":[{"seqId":"101","prevSeqId":"100",)"
                             R"("bids":[["50000","3.0","0","1"]],"asks":[],"ts":"999"}]})";
    EXPECT_EQ(handler_->process_message(update_msg), Result::SUCCESS);
    EXPECT_TRUE(deltas_.empty()); // still buffered

    // WS snapshot with seqId=100; the buffered update (prevSeqId=100) must replay.
    std::string snap_msg =
        R"({"action":"snapshot","arg":{"channel":"books","instId":"BTC-USDT"},)"
        R"("data":[{"bids":[["50000","1.0","0","1"]],"asks":[["50001","1.5","0","1"]],)"
        R"("ts":"1000","seqId":100}]})";
    EXPECT_EQ(handler_->process_message(snap_msg), Result::SUCCESS);

    EXPECT_EQ(handler_->get_sequence(), 101u); // buffered update was applied
    EXPECT_EQ(snapshot_count_, 1);
    EXPECT_FALSE(deltas_.empty()); // delta callback fired from buffered replay
}

// A delta carrying the correct CRC32 (computed from the exact wire strings stored
// during WS snapshot init) must pass validate_checksum and return SUCCESS.
TEST_F(OkxFeedHandlerTest, ValidChecksumPassesValidation) {
    std::string snap_msg =
        R"({"action":"snapshot","arg":{"channel":"books","instId":"BTC-USDT"},)"
        R"("data":[{"bids":[["50000","1.0","0","1"]],"asks":[["50001","1.5","0","1"]],)"
        R"("ts":"1000","seqId":100}]})";
    ASSERT_EQ(handler_->process_message(snap_msg), Result::SUCCESS);

    // After the delta: book is bid "50000":"2.0", ask "50001":"1.5" (ask unchanged).
    // OKX CRC32 input (top-1 bid interleaved with top-1 ask): "50000:2.0:50001:1.5"
    const int32_t expected_checksum =
        static_cast<int32_t>(OkxFeedHandler::crc32_for_test("50000:2.0:50001:1.5"));

    std::string update_msg =
        std::string(R"({"action":"update","arg":{"channel":"books","instId":"BTC-USDT"},)") +
        R"("data":[{"seqId":"101","prevSeqId":"100",)" +
        R"("bids":[["50000","2.0","0","1"]],"asks":[],"ts":"1001",)" +
        "\"checksum\":" + std::to_string(expected_checksum) + "}]}";

    EXPECT_EQ(handler_->process_message(update_msg), Result::SUCCESS);
    EXPECT_EQ(handler_->get_sequence(), 101u);
    EXPECT_FALSE(deltas_.empty());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
