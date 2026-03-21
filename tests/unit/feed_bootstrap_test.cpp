#include "core/engine/feed_bootstrap.hpp"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

struct TempFile {
    explicit TempFile(const std::string& suffix = ".bin") {
        char tmpl[] = "/tmp/feed_bootstrap_test_XXXXXX";
        int fd = ::mkstemp(tmpl);
        EXPECT_GE(fd, 0);
        ::close(fd);
        path = std::string(tmpl) + suffix;
        ::rename(tmpl, path.c_str());
    }

    ~TempFile() { ::unlink(path.c_str()); }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    std::string path;
};

class FakeFeedHandler {
  public:
    using SnapshotCallback = std::function<void(const trading::Snapshot&)>;
    using DeltaCallback = std::function<void(const trading::Delta&)>;

    explicit FakeFeedHandler(
        double tick_size, trading::Result refresh_result = trading::Result::SUCCESS)
        : tick_size_(tick_size), refresh_result_(refresh_result) {}

    void set_snapshot_callback(SnapshotCallback cb) { snapshot_callback_ = std::move(cb); }
    void set_delta_callback(DeltaCallback cb) { delta_callback_ = std::move(cb); }

    auto refresh_tick_size() -> trading::Result {
        ++refresh_calls_;
        return refresh_result_;
    }

    auto start() -> trading::Result {
        start_called_ = true;
        callbacks_wired_before_start_ = static_cast<bool>(snapshot_callback_) &&
                                        static_cast<bool>(delta_callback_);
        if (snapshot_callback_) {
            trading::Snapshot snapshot;
            snapshot.symbol = "BTCUSDT";
            snapshot.exchange = trading::Exchange::BINANCE;
            snapshot.sequence = 100;
            snapshot.timestamp_local_ns = 123'456'789LL;
            snapshot.bids = {{50'000.0, 2.0}, {49'999.0, 1.5}};
            snapshot.asks = {{50'001.0, 1.0}, {50'002.0, 1.2}};
            snapshot_callback_(snapshot);
        }
        return trading::Result::SUCCESS;
    }

    auto tick_size() const noexcept -> double { return tick_size_; }
    auto refresh_calls() const noexcept -> int { return refresh_calls_; }
    auto start_called() const noexcept -> bool { return start_called_; }
    auto callbacks_wired_before_start() const noexcept -> bool {
        return callbacks_wired_before_start_;
    }

  private:
    double tick_size_{0.0};
    trading::Result refresh_result_{trading::Result::SUCCESS};
    int refresh_calls_{0};
    bool start_called_{false};
    bool callbacks_wired_before_start_{false};
    SnapshotCallback snapshot_callback_;
    DeltaCallback delta_callback_;
};

TEST(FeedBootstrap, RefreshTickSizeForBookInitUsesExchangeValueWhenAvailable) {
    FakeFeedHandler feed(0.01);

    const double tick_size = trading::engine::refresh_tick_size_for_book_init(
        feed, true, "BINANCE", "BTCUSDT");

    EXPECT_DOUBLE_EQ(tick_size, 0.01);
    EXPECT_EQ(feed.refresh_calls(), 1);
}

TEST(FeedBootstrap, RefreshTickSizeForBookInitFallsBackWhenMetadataFetchFails) {
    FakeFeedHandler feed(0.0, trading::Result::ERROR_CONNECTION_LOST);

    const double tick_size = trading::engine::refresh_tick_size_for_book_init(
        feed, true, "BINANCE", "BTCUSDT");

    EXPECT_DOUBLE_EQ(tick_size, trading::engine::k_default_fallback_tick_size);
    EXPECT_EQ(feed.refresh_calls(), 1);
}

TEST(FeedBootstrap, WireCallbacksAndBridgeBeforeStartPublishesBootstrapSnapshot) {
    TempFile tmp;
    ::unlink(tmp.path.c_str());

    trading::LobPublisher publisher(tmp.path);
    ASSERT_TRUE(publisher.open());

    FakeFeedHandler feed(0.01);
    trading::BookManager book("BTCUSDT", trading::Exchange::BINANCE, 0.01, 200000);

    trading::engine::wire_book_bridge_and_callbacks(feed, book, &publisher);

    ASSERT_EQ(
        trading::engine::start_feed_after_wiring(feed, true, "BINANCE", "BTCUSDT"),
        trading::Result::SUCCESS);

    EXPECT_TRUE(feed.start_called());
    EXPECT_TRUE(feed.callbacks_wired_before_start());
    EXPECT_TRUE(book.is_ready());

    const size_t total = trading::LobPublisher::k_header_size +
                         trading::LobPublisher::k_capacity * trading::LobPublisher::k_slot_size;
    int fd = ::open(tmp.path.c_str(), O_RDONLY);
    ASSERT_GE(fd, 0);
    const char* mapped =
        static_cast<const char*>(::mmap(nullptr, total, PROT_READ, MAP_SHARED, fd, 0));
    ASSERT_NE(mapped, MAP_FAILED);

    const uint64_t* write_seq_ptr = reinterpret_cast<const uint64_t*>(mapped + 24);
    EXPECT_EQ(*write_seq_ptr, 1u);

    const char* slot0 = mapped + trading::LobPublisher::k_header_size;

    int64_t timestamp_ns = 0;
    std::memcpy(&timestamp_ns, slot0 + 16, sizeof(timestamp_ns));
    EXPECT_EQ(timestamp_ns, 123'456'789LL);

    double best_bid = 0.0;
    std::memcpy(&best_bid, slot0 + 32, sizeof(best_bid));
    EXPECT_DOUBLE_EQ(best_bid, 50'000.0);

    double best_ask = 0.0;
    std::memcpy(&best_ask, slot0 + 112, sizeof(best_ask));
    EXPECT_DOUBLE_EQ(best_ask, 50'001.0);

    ::munmap(const_cast<char*>(mapped), total);
    ::close(fd);
}

} // namespace
