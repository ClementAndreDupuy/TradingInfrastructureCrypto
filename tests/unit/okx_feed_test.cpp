#include "core/common/symbol_mapper.hpp"
#include "core/feeds/okx/okx_feed_handler.hpp"
#include <gtest/gtest.h>
#include <stdexcept>

using namespace trading;

class OkxFeedHandlerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        set_log_level(LogLevel::ERROR);
        handler_ = std::make_unique<OkxFeedHandler>("BTC-USDT", "https://www.okx.com",
                                                    "wss://ws.okx.com:8443/ws/v5/public");

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
    EXPECT_EQ(handler_->tick_size(), 0.0);
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

    std::string msg =
        R"({"arg":{"channel":"books","instId":"BTC-USDT"},"data":[{"seqId":"104","prevSeqId":"103","bids":[["50000","1.0","0","1"]],"asks":[["50001","1.0","0","1"]]}]})";
    EXPECT_EQ(handler_->process_message(msg), Result::ERROR_SEQUENCE_GAP);
}

TEST_F(OkxFeedHandlerTest, StaleUpdateRejectedInStreamingState) {
    handler_->set_streaming_state_for_test(100);
    handler_->seed_book_state_for_test({PriceLevel{50000.0, 1.0}}, {PriceLevel{50001.0, 1.0}});

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
    OkxFeedHandler handler("BTCUSDT", "https://www.okx.com", "wss://ws.okx.com:8443/ws/v5/public");
    handler.set_streaming_state_for_test(100);
    handler.seed_book_state_for_test({PriceLevel{50000.0, 1.0}}, {PriceLevel{50001.0, 1.0}});

    std::vector<Delta> deltas;
    handler.set_delta_callback([&deltas](const Delta& delta) { deltas.push_back(delta); });

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

TEST_F(OkxFeedHandlerTest, BufferedUpdateAppliedAfterWsSnapshot) {
    std::string update_msg = R"({"action":"update","arg":{"channel":"books","instId":"BTC-USDT"},)"
                             R"("data":[{"seqId":"101","prevSeqId":"100",)"
                             R"("bids":[["50000","3.0","0","1"]],"asks":[],"ts":"999"}]})";
    EXPECT_EQ(handler_->process_message(update_msg), Result::SUCCESS);
    EXPECT_TRUE(deltas_.empty());

    std::string snap_msg =
        R"({"action":"snapshot","arg":{"channel":"books","instId":"BTC-USDT"},)"
        R"("data":[{"bids":[["50000","1.0","0","1"]],"asks":[["50001","1.5","0","1"]],)"
        R"("ts":"1000","seqId":100}]})";
    EXPECT_EQ(handler_->process_message(snap_msg), Result::SUCCESS);

    EXPECT_EQ(handler_->get_sequence(), 101u);
    EXPECT_EQ(snapshot_count_, 1);
    EXPECT_FALSE(deltas_.empty());
}

TEST_F(OkxFeedHandlerTest, ValidChecksumPassesValidation) {
    std::string snap_msg =
        R"({"action":"snapshot","arg":{"channel":"books","instId":"BTC-USDT"},)"
        R"("data":[{"bids":[["50000","1.0","0","1"]],"asks":[["50001","1.5","0","1"]],)"
        R"("ts":"1000","seqId":100}]})";
    ASSERT_EQ(handler_->process_message(snap_msg), Result::SUCCESS);

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
