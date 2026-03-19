#include "core/feeds/binance/binance_feed_handler.hpp"
#include "core/feeds/coinbase/coinbase_feed_handler.hpp"
#include "core/feeds/kraken/kraken_feed_handler.hpp"
#include "core/feeds/okx/okx_feed_handler.hpp"

#include <gtest/gtest.h>

namespace trading {
namespace {

TEST(FeedDisconnectChaosTest, BinanceGapThenRecoveryMessageSucceeds) {
    BinanceFeedHandler h("BTCUSDT");
    // Stay offline: deterministic gap/recovery validation without network I/O.
    const uint64_t base = h.get_sequence();

    const std::string gap = R"({"e":"depthUpdate","s":"BTCUSDT","U":)" +
                            std::to_string(base + 100) + R"(,"u":)" + std::to_string(base + 100) +
                            R"(,"b":[],"a":[]})";
    EXPECT_EQ(h.process_message(gap), Result::ERROR_SEQUENCE_GAP);

    const std::string recover = R"({"e":"depthUpdate","s":"BTCUSDT","U":)" +
                                std::to_string(base + 1) + R"(,"u":)" + std::to_string(base + 1) +
                                R"(,"b":[["50000.0","1.0"]],"a":[["50001.0","1.0"]]})";
    EXPECT_EQ(h.process_message(recover), Result::SUCCESS);
}

TEST(FeedDisconnectChaosTest, KrakenGapThenRecoveryMessageSucceeds) {
    KrakenFeedHandler h("XBTUSD");
    const uint32_t snap_checksum =
        KrakenFeedHandler::crc32_for_test("500011000000050000100000000");
    const std::string snapshot =
        std::string(R"({"channel":"book","type":"snapshot","data":[{"symbol":"BTC/USD","bids":[{"price":"50000.0","qty":"1.00000000"}],"asks":[{"price":"50001.0","qty":"1.00000000"}],"checksum":)") +
        std::to_string(snap_checksum) + R"(,"timestamp":"2024-01-01T00:00:00Z"}]})";
    ASSERT_EQ(h.process_message(snapshot), Result::SUCCESS);

    const uint32_t update_checksum =
        KrakenFeedHandler::crc32_for_test("500005150000000500011000000050000100000000");
    const std::string recover =
        std::string(R"({"channel":"book","type":"update","data":[{"symbol":"BTC/USD","bids":[],"asks":[{"price":"50000.5","qty":"1.50000000"}],"checksum":)") +
        std::to_string(update_checksum) + R"(,"timestamp":"2024-01-01T00:00:01Z"}]})";
    EXPECT_EQ(h.process_message(recover), Result::SUCCESS);
}

TEST(FeedDisconnectChaosTest, CoinbaseGapThenRecoveryViaSnapshot) {
    CoinbaseFeedHandler h("BTC-USD");

    const std::string snap =
        R"({"channel":"l2_data","type":"l2_data","sequence_num":100,"events":[{"type":"snapshot","updates":[{"side":"bid","price_level":"50000.0","new_quantity":"1.0"},{"side":"offer","price_level":"50001.0","new_quantity":"1.0"}]}]})";
    ASSERT_EQ(h.process_message(snap), Result::SUCCESS);

    const std::string gap =
        R"({"channel":"l2_data","type":"l2_data","sequence_num":103,"events":[{"type":"update","updates":[{"side":"bid","price_level":"50000.5","new_quantity":"1.0"}]}]})";
    EXPECT_EQ(h.process_message(gap), Result::ERROR_SEQUENCE_GAP);

    const std::string resnap =
        R"({"channel":"l2_data","type":"l2_data","sequence_num":200,"events":[{"type":"snapshot","updates":[{"side":"bid","price_level":"50010.0","new_quantity":"1.0"},{"side":"offer","price_level":"50011.0","new_quantity":"1.0"}]}]})";
    EXPECT_EQ(h.process_message(resnap), Result::SUCCESS);

    const std::string update =
        R"({"channel":"l2_data","type":"l2_data","sequence_num":201,"events":[{"type":"update","updates":[{"side":"bid","price_level":"50010.5","new_quantity":"1.5"}]}]})";
    EXPECT_EQ(h.process_message(update), Result::SUCCESS);
}

TEST(FeedDisconnectChaosTest, OkxGapThenRecoveryWithStreamingState) {
    OkxFeedHandler h("BTC-USDT");
    h.set_streaming_state_for_test(100);
    h.seed_book_state_for_test({PriceLevel{50000.0, 1.0}}, {PriceLevel{50001.0, 1.0}});

    const std::string gap =
        R"({"arg":{"channel":"books","instId":"BTC-USDT"},"data":[{"seqId":"105","prevSeqId":"104","bids":[["50000","1.0","0","1"]],"asks":[["50001","1.0","0","1"]]}]})";
    EXPECT_EQ(h.process_message(gap), Result::ERROR_SEQUENCE_GAP);

    h.set_streaming_state_for_test(100);
    h.seed_book_state_for_test({PriceLevel{50000.0, 1.0}}, {PriceLevel{50001.0, 1.0}});
    const std::string recover =
        R"({"arg":{"channel":"books","instId":"BTC-USDT"},"data":[{"seqId":"101","prevSeqId":"100","bids":[["50000","1.0","0","1"]],"asks":[["50001","1.0","0","1"]]}]})";
    EXPECT_EQ(h.process_message(recover), Result::SUCCESS);
}

} // namespace
} // namespace trading
