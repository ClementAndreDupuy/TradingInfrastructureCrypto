#include "core/feeds/common/book_manager.hpp"
#include "core/feeds/kraken/kraken_feed_handler.hpp"
#include <gtest/gtest.h>
#include <string>

using namespace trading;

namespace {

std::string normalize_field(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        if (ch != '.') {
            out.push_back(ch);
        }
    }
    const size_t first = out.find_first_not_of('0');
    return first == std::string::npos ? "0" : out.substr(first);
}

std::string checksum_payload(const std::vector<std::pair<std::string, std::string>>& asks,
                             const std::vector<std::pair<std::string, std::string>>& bids) {
    std::string payload;
    for (const auto& [price, qty] : asks) {
        payload += normalize_field(price);
        payload += normalize_field(qty);
    }
    for (const auto& [price, qty] : bids) {
        payload += normalize_field(price);
        payload += normalize_field(qty);
    }
    return payload;
}

uint32_t snapshot_checksum() {
    return KrakenFeedHandler::crc32_for_test(checksum_payload(
        {{"50001.0", "2.00000000"}, {"50002.0", "3.50000000"}},
        {{"50000.0", "1.50000000"}, {"49999.5", "0.75000000"}}));
}

uint32_t update_checksum_after_insert() {
    return KrakenFeedHandler::crc32_for_test(checksum_payload(
        {{"50000.5", "1.10000000"}, {"50001.0", "2.00000000"}, {"50002.0", "3.50000000"}},
        {{"50000.0", "1.50000000"}, {"49999.5", "0.75000000"}}));
}

}

class KrakenFeedHandlerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        set_log_level(LogLevel::ERROR);
        handler_ = std::make_unique<KrakenFeedHandler>("XBTUSD", "https://api.kraken.com",
                                                         "wss://ws.kraken.com/v2");

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

    std::string valid_snapshot_message() const {
        return std::string(R"({"channel":"book","type":"snapshot","data":[{"symbol":"BTC/USD","bids":[{"price":"50000.0","qty":"1.50000000"},{"price":"49999.5","qty":"0.75000000"}],"asks":[{"price":"50001.0","qty":"2.00000000"},{"price":"50002.0","qty":"3.50000000"}],"checksum":)") +
               std::to_string(snapshot_checksum()) +
               R"(,"timestamp":"2024-01-01T00:00:00.123456789Z"}]})";
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
    EXPECT_EQ(handler_->get_sequence(), 0u);
    EXPECT_EQ(handler_->tick_size(), 0.0);
}

TEST_F(KrakenFeedHandlerTest, TickSizePositiveAfterStart) {
    ASSERT_EQ(handler_->start(), Result::SUCCESS);
    EXPECT_GT(handler_->tick_size(), 0.0);
}

TEST_F(KrakenFeedHandlerTest, HandlerCreationWithCustomEndpoints) {
    auto h = std::make_unique<KrakenFeedHandler>("XBTUSD", "https://api.kraken.com",
                                                 "wss://ws.kraken.com/v2");
    EXPECT_NE(h, nullptr);
    EXPECT_FALSE(h->is_running());
}

TEST_F(KrakenFeedHandlerTest, StartHandler) {
    EXPECT_EQ(handler_->start(), Result::SUCCESS);
    EXPECT_TRUE(handler_->is_running());
    EXPECT_EQ(handler_->get_sequence(), 0u);
    EXPECT_EQ(snapshot_count_, 1);
    EXPECT_EQ(last_snapshot_.exchange, Exchange::KRAKEN);
    EXPECT_FALSE(last_snapshot_.bids.empty());
    EXPECT_FALSE(last_snapshot_.asks.empty());
}

TEST_F(KrakenFeedHandlerTest, WebsocketSnapshotInitializesBook) {
    EXPECT_EQ(handler_->process_message(valid_snapshot_message()), Result::SUCCESS);
    EXPECT_EQ(snapshot_count_, 1);
    EXPECT_EQ(last_snapshot_.exchange, Exchange::KRAKEN);
    EXPECT_EQ(last_snapshot_.sequence, 0u);
    EXPECT_FALSE(last_snapshot_.checksum_present);
    EXPECT_EQ(last_snapshot_.checksum, 0u);
    EXPECT_EQ(last_snapshot_.timestamp_exchange_ns, 1704067200123456789LL);
    EXPECT_GT(last_snapshot_.timestamp_local_ns, 0);
    ASSERT_EQ(last_snapshot_.bids.size(), 2u);
    ASSERT_EQ(last_snapshot_.asks.size(), 2u);
    EXPECT_DOUBLE_EQ(last_snapshot_.bids[0].price, 50000.0);
    EXPECT_DOUBLE_EQ(last_snapshot_.asks[0].price, 50001.0);
}

TEST_F(KrakenFeedHandlerTest, UpdateWithoutVenueSequenceUsesLocalOrdering) {
    ASSERT_EQ(handler_->process_message(valid_snapshot_message()), Result::SUCCESS);
    deltas_.clear();

    std::string msg = std::string(
        R"({"channel":"book","type":"update","data":[{"symbol":"BTC/USD","bids":[],"asks":[{"price":"50000.5","qty":"1.10000000"}],"checksum":)") +
        std::to_string(update_checksum_after_insert()) +
        R"(,"timestamp":"2024-01-01T00:00:01.000000001Z"}]})";

    EXPECT_EQ(handler_->process_message(msg), Result::SUCCESS);
    EXPECT_EQ(handler_->get_sequence(), 1u);
    ASSERT_EQ(deltas_.size(), 1u);
    EXPECT_EQ(deltas_[0].side, Side::ASK);
    EXPECT_DOUBLE_EQ(deltas_[0].price, 50000.5);
    EXPECT_DOUBLE_EQ(deltas_[0].size, 1.1);
    EXPECT_EQ(deltas_[0].sequence, 1u);
    EXPECT_EQ(deltas_[0].timestamp_exchange_ns, 1704067201000000001LL);
    EXPECT_GT(deltas_[0].timestamp_local_ns, 0);
}

TEST_F(KrakenFeedHandlerTest, NumericWirePrecisionStillPassesChecksum) {
    std::string snapshot = std::string(
        R"({"channel":"book","type":"snapshot","data":[{"symbol":"BTC/USD","bids":[{"price":50000.0,"qty":1.50000000},{"price":49999.5,"qty":0.75000000}],"asks":[{"price":50001.0,"qty":2.00000000},{"price":50002.0,"qty":3.50000000}],"checksum":)") +
                           std::to_string(snapshot_checksum()) +
                           R"(,"timestamp":"2024-01-01T00:00:00.123456789Z"}]})";

    EXPECT_EQ(handler_->process_message(snapshot), Result::SUCCESS);
    EXPECT_EQ(snapshot_count_, 1);
}

TEST(KrakenBookManagerRegression, VenueChecksumDoesNotTripGenericOrderBookValidation) {
    set_log_level(LogLevel::ERROR);

    BookManager book("BTCUSDT", Exchange::KRAKEN, 0.5, 200000);
    KrakenFeedHandler handler("XBTUSD", "https://api.kraken.com", "wss://ws.kraken.com/v2");
    handler.set_snapshot_callback(book.snapshot_handler());

    const std::string snapshot = std::string(
        R"({"channel":"book","type":"snapshot","data":[{"symbol":"BTC/USD","bids":[{"price":"50000.0","qty":"1.50000000"},{"price":"49999.5","qty":"0.75000000"}],"asks":[{"price":"50001.0","qty":"2.00000000"},{"price":"50002.0","qty":"3.50000000"}],"checksum":)") +
        std::to_string(snapshot_checksum()) +
        R"(,"timestamp":"2024-01-01T00:00:00.123456789Z"}]})";

    EXPECT_EQ(handler.process_message(snapshot), Result::SUCCESS);
    EXPECT_TRUE(book.is_ready());
    EXPECT_DOUBLE_EQ(book.best_bid(), 50000.0);
    EXPECT_DOUBLE_EQ(book.best_ask(), 50001.0);
}

TEST_F(KrakenFeedHandlerTest, ChecksumMismatchTriggersResnapshot) {
    ASSERT_EQ(handler_->process_message(valid_snapshot_message()), Result::SUCCESS);

    std::string msg = R"({"channel":"book","type":"update","data":[{"symbol":"BTC/USD","bids":[],"asks":[{"price":"50000.5","qty":"1.10000000"}],"checksum":1,"timestamp":"2024-01-01T00:00:01Z"}]})";

    EXPECT_EQ(handler_->process_message(msg), Result::ERROR_BOOK_CORRUPTED);
    EXPECT_EQ(handler_->get_sequence(), 0u);
    EXPECT_FALSE(last_error_.empty());
}

TEST_F(KrakenFeedHandlerTest, BufferedUpdatesReplayAfterSnapshot) {
    std::string msg = std::string(
        R"({"channel":"book","type":"update","data":[{"symbol":"BTC/USD","bids":[],"asks":[{"price":"50000.5","qty":"1.10000000"}],"checksum":)") +
        std::to_string(update_checksum_after_insert()) +
        R"(,"timestamp":"2024-01-01T00:00:01Z"}]})";

    EXPECT_EQ(handler_->process_message(msg), Result::SUCCESS);
    EXPECT_TRUE(deltas_.empty());
    EXPECT_EQ(handler_->process_message(valid_snapshot_message()), Result::SUCCESS);
    EXPECT_EQ(handler_->get_sequence(), 1u);
    ASSERT_EQ(deltas_.size(), 1u);
    EXPECT_DOUBLE_EQ(deltas_[0].price, 50000.5);
}

TEST_F(KrakenFeedHandlerTest, LocalBookTruncatesToSubscribedDepth) {
    handler_->seed_book_state_for_test({}, {});
    handler_->set_streaming_state_for_test(0);

    std::vector<std::pair<std::string, std::string>> asks;
    asks.reserve(101);
    for (int i = 0; i < 101; ++i) {
        asks.emplace_back(std::to_string(50001 + i) + ".0", "1.00000000");
    }
    std::vector<std::pair<std::string, std::string>> bids = {{"50000.0", "1.00000000"}};

    std::string ask_json;
    for (size_t i = 0; i < asks.size(); ++i) {
        if (i > 0) {
            ask_json += ',';
        }
        ask_json += std::string(R"({"price":")") + asks[i].first + R"(","qty":")" +
                    asks[i].second + R"("})";
    }
    const uint32_t checksum = KrakenFeedHandler::crc32_for_test(checksum_payload(
        {asks.begin(), asks.begin() + 10}, bids));

    std::string msg = std::string(R"({"channel":"book","type":"update","data":[{"symbol":"BTC/USD","bids":[{"price":"50000.0","qty":"1.00000000"}],"asks":[)") +
                      ask_json + R"(],"checksum":)" + std::to_string(checksum) +
                      R"(,"timestamp":"2024-01-01T00:00:01Z"}]})";

    EXPECT_EQ(handler_->process_message(msg), Result::SUCCESS);
    EXPECT_EQ(handler_->ask_depth_for_test(), 100u);
    EXPECT_EQ(handler_->bid_depth_for_test(), 1u);
}

TEST_F(KrakenFeedHandlerTest, IgnoreNonBookMessages) {
    size_t before = deltas_.size();
    std::string msg = R"({"channel":"heartbeat","timestamp":"2024-01-01T00:00:00Z"})";
    EXPECT_EQ(handler_->process_message(msg), Result::SUCCESS);
    EXPECT_EQ(deltas_.size(), before);
}

TEST_F(KrakenFeedHandlerTest, SubscribeErrorTriggersResnapshot) {
    std::string msg =
        R"({"method":"subscribe","success":false,"error":"EGeneral:invalid arguments"})";
    EXPECT_EQ(handler_->process_message(msg), Result::ERROR_CONNECTION_LOST);
    EXPECT_FALSE(last_error_.empty());
}

TEST_F(KrakenFeedHandlerTest, MalformedJsonIsIgnored) {
    EXPECT_EQ(handler_->process_message("{invalid"), Result::SUCCESS);
}

TEST_F(KrakenFeedHandlerTest, ExtremePriceLevelAcceptedAsDataPoint) {
    ASSERT_EQ(handler_->process_message(valid_snapshot_message()), Result::SUCCESS);
    handler_->seed_book_state_for_test({{1000000000.0, 1.0}}, {{0.00000001, 1.0}});
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
    ASSERT_EQ(handler_->process_message(valid_snapshot_message()), Result::SUCCESS);
    EXPECT_EQ(last_snapshot_.exchange, Exchange::KRAKEN);
    EXPECT_EQ(std::string(exchange_to_string(Exchange::KRAKEN)), "KRAKEN");
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
