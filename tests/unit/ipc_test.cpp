// Unit tests for core/ipc/alpha_signal.hpp and core/ipc/lob_publisher.hpp.
//
// Tests write synthetic mmap files on disk (under the OS temp directory) and
// verify the C++ readers parse them correctly without requiring the live Python
// signal publisher or C++ feed handlers to be running.

#include "core/ipc/alpha_signal.hpp"
#include "core/ipc/lob_publisher.hpp"
#include "core/feeds/common/book_manager.hpp"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

namespace {

// ── Helpers ──────────────────────────────────────────────────────────────────

// Returns a unique temp path that is removed when the TempFile is destroyed.
struct TempFile {
    explicit TempFile(const std::string& suffix = ".bin") {
        char tmpl[] = "/tmp/ipc_test_XXXXXX";
        int fd = ::mkstemp(tmpl);
        EXPECT_GE(fd, 0);
        ::close(fd);
        path = std::string(tmpl) + suffix;
        // mkstemp already created the base name; rename to add suffix.
        ::rename(tmpl, path.c_str());
    }
    ~TempFile() { ::unlink(path.c_str()); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    std::string path;
};

// Write a seqlock-formatted alpha signal file for testing.
// Layout: [uint64 seq][float64 signal_bps][float64 risk_score][int64 ts_ns]
void write_signal_file(const std::string& path, uint64_t seq, double signal_bps,
                       double risk_score, int64_t ts_ns) {
    // Truncate/create file at exact size.
    constexpr size_t SIZE = trading::AlphaSignalReader::FILE_SIZE; // 32
    int fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(::ftruncate(fd, static_cast<off_t>(SIZE)), 0);

    void* m = ::mmap(nullptr, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT_NE(m, MAP_FAILED);
    char* p = static_cast<char*>(m);

    std::memcpy(p,      &seq,        8);
    std::memcpy(p + 8,  &signal_bps, 8);
    std::memcpy(p + 16, &risk_score, 8);
    std::memcpy(p + 24, &ts_ns,      8);

    ::munmap(m, SIZE);
    ::close(fd);
}

// ── AlphaSignalReader tests ───────────────────────────────────────────────────

TEST(AlphaSignalReader, OpenNonExistentFileReturnsFalse) {
    trading::AlphaSignalReader reader("/tmp/no_such_file_ipc_test.bin");
    EXPECT_FALSE(reader.open());
    EXPECT_FALSE(reader.is_open());
}

TEST(AlphaSignalReader, ReadOnClosedReaderReturnsNeutral) {
    trading::AlphaSignalReader reader("/tmp/no_such_file_ipc_test.bin");
    // No open() call — ptr_ is null.
    trading::AlphaSignal sig = reader.read();
    EXPECT_DOUBLE_EQ(sig.signal_bps, 0.0);
    EXPECT_DOUBLE_EQ(sig.risk_score, 0.5);
    EXPECT_EQ(sig.ts_ns, 0);
}

TEST(AlphaSignalReader, FailOpenWhenNotOpen) {
    trading::AlphaSignalReader reader("/tmp/no_such_file_ipc_test.bin");
    EXPECT_TRUE(reader.allows_long());
    EXPECT_TRUE(reader.allows_short());
    EXPECT_TRUE(reader.allows_mm());
}

TEST(AlphaSignalReader, ReadSeqlockStableEvenSeq) {
    TempFile tmp;

    using namespace std::chrono;
    const int64_t ts_now =
        duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();

    // seq = 2 (even → stable)
    write_signal_file(tmp.path, /*seq=*/2, /*signal_bps=*/8.5,
                      /*risk_score=*/0.3, ts_now);

    trading::AlphaSignalReader reader(tmp.path, /*signal_min_bps=*/3.0,
                                      /*risk_max=*/0.65);
    ASSERT_TRUE(reader.open());

    trading::AlphaSignal sig = reader.read();
    EXPECT_NEAR(sig.signal_bps, 8.5, 1e-9);
    EXPECT_NEAR(sig.risk_score, 0.3, 1e-9);
    EXPECT_EQ(sig.ts_ns, ts_now);
}

TEST(AlphaSignalReader, ReadSeqlockOddSeqReturnsNeutral) {
    TempFile tmp;

    using namespace std::chrono;
    const int64_t ts_now =
        duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();

    // seq = 1 (odd → writer active): reader must fail-open after retries.
    write_signal_file(tmp.path, /*seq=*/1, /*signal_bps=*/8.5,
                      /*risk_score=*/0.3, ts_now);

    trading::AlphaSignalReader reader(tmp.path, /*signal_min_bps=*/3.0,
                                      /*risk_max=*/0.65);
    ASSERT_TRUE(reader.open());

    // With an odd seq that never becomes even, read() returns {} after
    // exhausting retries.  ts_ns == 0 → is_stale → fail-open.
    trading::AlphaSignal sig = reader.read();
    EXPECT_EQ(sig.ts_ns, 0); // neutral / fail-open signal
}

TEST(AlphaSignalReader, AllowsLongWithFreshBullishSignal) {
    TempFile tmp;

    using namespace std::chrono;
    const int64_t ts_now =
        duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();

    write_signal_file(tmp.path, 0, 10.0, 0.2, ts_now);

    trading::AlphaSignalReader reader(tmp.path, 3.0, 0.65);
    ASSERT_TRUE(reader.open());
    EXPECT_TRUE(reader.allows_long());
    EXPECT_FALSE(reader.allows_short());
    EXPECT_TRUE(reader.allows_mm());
}

TEST(AlphaSignalReader, AllowsShortWithFreshBearishSignal) {
    TempFile tmp;

    using namespace std::chrono;
    const int64_t ts_now =
        duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();

    write_signal_file(tmp.path, 0, -10.0, 0.2, ts_now);

    trading::AlphaSignalReader reader(tmp.path, 3.0, 0.65);
    ASSERT_TRUE(reader.open());
    EXPECT_FALSE(reader.allows_long());
    EXPECT_TRUE(reader.allows_short());
    EXPECT_TRUE(reader.allows_mm());
}

TEST(AlphaSignalReader, BlockedByHighRiskScore) {
    TempFile tmp;

    using namespace std::chrono;
    const int64_t ts_now =
        duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();

    // Strong bullish signal but high adverse-selection risk.
    write_signal_file(tmp.path, 0, 10.0, 0.9, ts_now);

    trading::AlphaSignalReader reader(tmp.path, 3.0, 0.65);
    ASSERT_TRUE(reader.open());
    EXPECT_FALSE(reader.allows_long());
    EXPECT_FALSE(reader.allows_short());
    EXPECT_FALSE(reader.allows_mm());
}

TEST(AlphaSignalReader, FailOpenOnStaleSignal) {
    TempFile tmp;

    // ts_ns = 1 (epoch, far in the past → stale).
    write_signal_file(tmp.path, 0, 10.0, 0.9, /*ts_ns=*/1LL);

    trading::AlphaSignalReader reader(tmp.path, 3.0, 0.65);
    ASSERT_TRUE(reader.open());
    // Stale signal → fail-open regardless of risk_score / signal_bps.
    EXPECT_TRUE(reader.allows_long());
    EXPECT_TRUE(reader.allows_short());
    EXPECT_TRUE(reader.allows_mm());
}

TEST(AlphaSignalReader, FailOpenOnTsNsZero) {
    TempFile tmp;
    write_signal_file(tmp.path, 0, 10.0, 0.9, /*ts_ns=*/0LL);

    trading::AlphaSignalReader reader(tmp.path, 3.0, 0.65);
    ASSERT_TRUE(reader.open());
    EXPECT_TRUE(reader.allows_long());
    EXPECT_TRUE(reader.allows_short());
    EXPECT_TRUE(reader.allows_mm());
}

TEST(AlphaSignalReader, CloseIsIdempotent) {
    TempFile tmp;
    write_signal_file(tmp.path, 0, 1.0, 0.1, 1LL);

    trading::AlphaSignalReader reader(tmp.path);
    ASSERT_TRUE(reader.open());
    reader.close();
    reader.close(); // must not crash or double-close fd
    EXPECT_FALSE(reader.is_open());
}

// ── LobPublisher tests ────────────────────────────────────────────────────────

TEST(LobPublisher, OpenCreatesFile) {
    TempFile tmp;
    ::unlink(tmp.path.c_str()); // ensure it does not pre-exist

    trading::LobPublisher pub(tmp.path);
    ASSERT_TRUE(pub.open());
    EXPECT_TRUE(pub.is_open());

    // File must exist and have the correct size.
    const size_t expected =
        trading::LobPublisher::k_header_size +
        trading::LobPublisher::k_capacity * trading::LobPublisher::k_slot_size;
    struct stat st;
    ASSERT_EQ(::stat(tmp.path.c_str(), &st), 0);
    EXPECT_EQ(static_cast<size_t>(st.st_size), expected);
}

TEST(LobPublisher, OpenReturnsTrueIfAlreadyOpen) {
    TempFile tmp;
    ::unlink(tmp.path.c_str());

    trading::LobPublisher pub(tmp.path);
    ASSERT_TRUE(pub.open());
    EXPECT_TRUE(pub.open()); // second call should be a no-op returning true
}

TEST(LobPublisher, CloseIsIdempotent) {
    TempFile tmp;
    ::unlink(tmp.path.c_str());

    trading::LobPublisher pub(tmp.path);
    ASSERT_TRUE(pub.open());
    pub.close();
    pub.close(); // must not crash
    EXPECT_FALSE(pub.is_open());
}

TEST(LobPublisher, PublishAdvancesWriteSeq) {
    TempFile tmp;
    ::unlink(tmp.path.c_str());

    trading::LobPublisher pub(tmp.path);
    ASSERT_TRUE(pub.open());

    // Map the header independently to read write_seq without going through pub.
    const size_t total = trading::LobPublisher::k_header_size +
                         trading::LobPublisher::k_capacity *
                             trading::LobPublisher::k_slot_size;
    int fd = ::open(tmp.path.c_str(), O_RDONLY);
    ASSERT_GE(fd, 0);
    void* m = ::mmap(nullptr, total, PROT_READ, MAP_SHARED, fd, 0);
    ASSERT_NE(m, MAP_FAILED);

    // write_seq is a uint64 at offset 24 within the 64-byte header
    // (magic[8] + version[4] + slot_size[4] + capacity[4] + reserved0[4] + write_seq[8]).
    const uint64_t* write_seq_ptr =
        reinterpret_cast<const uint64_t*>(static_cast<const char*>(m) + 24);

    EXPECT_EQ(*write_seq_ptr, 0u);

    std::vector<trading::PriceLevel> bids = {{100.0, 1.0}, {99.9, 2.0}};
    std::vector<trading::PriceLevel> asks = {{100.1, 1.0}, {100.2, 2.0}};
    pub.publish(trading::Exchange::BINANCE, "BTCUSDT", 1'000'000LL, 100.05, bids, asks);

    // Allow the release-store to propagate (same process, so it's already visible).
    std::atomic_thread_fence(std::memory_order_acquire);
    EXPECT_EQ(*write_seq_ptr, 1u);

    pub.publish(trading::Exchange::KRAKEN, "BTCUSDT", 2'000'000LL, 100.10, bids, asks);
    std::atomic_thread_fence(std::memory_order_acquire);
    EXPECT_EQ(*write_seq_ptr, 2u);

    ::munmap(m, total);
    ::close(fd);
}

TEST(LobPublisher, SlotContentsMatchPublishedData) {
    TempFile tmp;
    ::unlink(tmp.path.c_str());

    trading::LobPublisher pub(tmp.path);
    ASSERT_TRUE(pub.open());

    std::vector<trading::PriceLevel> bids = {
        {100.0, 1.5}, {99.9, 2.0}, {99.8, 3.0}, {99.7, 1.0}, {99.6, 0.5}};
    std::vector<trading::PriceLevel> asks = {
        {100.1, 1.5}, {100.2, 2.0}, {100.3, 3.0}, {100.4, 1.0}, {100.5, 0.5}};

    pub.publish(trading::Exchange::OKX, "BTCUSDT", 123'456'789LL, 100.05, bids, asks);

    // Read slot 0 directly from the file.
    const size_t total = trading::LobPublisher::k_header_size +
                         trading::LobPublisher::k_capacity *
                             trading::LobPublisher::k_slot_size;
    int fd = ::open(tmp.path.c_str(), O_RDONLY);
    ASSERT_GE(fd, 0);
    const char* m =
        static_cast<const char*>(::mmap(nullptr, total, PROT_READ, MAP_SHARED, fd, 0));
    ASSERT_NE(m, MAP_FAILED);

    const char* slot0 = m + trading::LobPublisher::k_header_size;

    uint8_t exchange_id;
    std::memcpy(&exchange_id, slot0, 1);
    EXPECT_EQ(exchange_id, static_cast<uint8_t>(trading::Exchange::OKX));

    int64_t ts_ns;
    std::memcpy(&ts_ns, slot0 + 16, 8); // after exchange_id(1) + symbol(15)
    EXPECT_EQ(ts_ns, 123'456'789LL);

    double mid;
    std::memcpy(&mid, slot0 + 24, 8);
    EXPECT_NEAR(mid, 100.05, 1e-9);

    // Verify best bid price (first element of bid_price[5] at offset 32).
    double best_bid;
    std::memcpy(&best_bid, slot0 + 32, 8);
    EXPECT_NEAR(best_bid, 100.0, 1e-9);

    // Verify best ask price (at offset 32 + 5*8 bid_price + 5*8 bid_size = 32+80 = 112).
    double best_ask;
    std::memcpy(&best_ask, slot0 + 112, 8);
    EXPECT_NEAR(best_ask, 100.1, 1e-9);

    ::munmap(const_cast<char*>(m), total);
    ::close(fd);
}

TEST(LobPublisher, BookManagerSnapshotPublishesImmediately) {
    TempFile tmp;
    ::unlink(tmp.path.c_str());

    trading::LobPublisher pub(tmp.path);
    ASSERT_TRUE(pub.open());

    trading::BookManager manager("BTCUSDT", trading::Exchange::BINANCE, 1.0, 1024);
    manager.set_publisher(&pub);

    trading::Snapshot snapshot;
    snapshot.symbol = "BTCUSDT";
    snapshot.exchange = trading::Exchange::BINANCE;
    snapshot.sequence = 42;
    snapshot.timestamp_local_ns = 987'654'321LL;
    snapshot.bids = {{50'000.0, 1.5}, {49'999.0, 2.0}};
    snapshot.asks = {{50'001.0, 1.0}, {50'002.0, 3.0}};

    auto on_snapshot = manager.snapshot_handler();
    on_snapshot(snapshot);

    const size_t total = trading::LobPublisher::k_header_size +
                         trading::LobPublisher::k_capacity *
                             trading::LobPublisher::k_slot_size;
    int fd = ::open(tmp.path.c_str(), O_RDONLY);
    ASSERT_GE(fd, 0);
    const char* m =
        static_cast<const char*>(::mmap(nullptr, total, PROT_READ, MAP_SHARED, fd, 0));
    ASSERT_NE(m, MAP_FAILED);

    const uint64_t* write_seq_ptr =
        reinterpret_cast<const uint64_t*>(m + 24);
    EXPECT_EQ(*write_seq_ptr, 1u);

    const char* slot0 = m + trading::LobPublisher::k_header_size;

    int64_t ts_ns;
    std::memcpy(&ts_ns, slot0 + 16, 8);
    EXPECT_EQ(ts_ns, snapshot.timestamp_local_ns);

    double best_bid;
    std::memcpy(&best_bid, slot0 + 32, 8);
    EXPECT_NEAR(best_bid, 50'000.0, 1e-9);

    double best_ask;
    std::memcpy(&best_ask, slot0 + 112, 8);
    EXPECT_NEAR(best_ask, 50'001.0, 1e-9);

    ::munmap(const_cast<char*>(m), total);
    ::close(fd);
}

TEST(LobPublisher, RingBufferWrapsAround) {
    TempFile tmp;
    ::unlink(tmp.path.c_str());

    trading::LobPublisher pub(tmp.path);
    ASSERT_TRUE(pub.open());

    std::vector<trading::PriceLevel> bids = {{100.0, 1.0}};
    std::vector<trading::PriceLevel> asks = {{100.1, 1.0}};

    // Write exactly k_capacity + 1 slots; the last write should land in slot 0,
    // overwriting the first entry.
    const uint64_t n = trading::LobPublisher::k_capacity + 1;
    for (uint64_t i = 0; i < n; ++i) {
        pub.publish(trading::Exchange::BINANCE, "BTCUSDT",
                    static_cast<int64_t>(i), 100.0 + static_cast<double>(i),
                    bids, asks);
    }

    // Check write_seq == n.
    const size_t total = trading::LobPublisher::k_header_size +
                         trading::LobPublisher::k_capacity *
                             trading::LobPublisher::k_slot_size;
    int fd = ::open(tmp.path.c_str(), O_RDONLY);
    ASSERT_GE(fd, 0);
    const char* m =
        static_cast<const char*>(::mmap(nullptr, total, PROT_READ, MAP_SHARED, fd, 0));
    ASSERT_NE(m, MAP_FAILED);

    std::atomic_thread_fence(std::memory_order_acquire);

    uint64_t write_seq;
    std::memcpy(&write_seq, m + 24, 8);
    EXPECT_EQ(write_seq, n);

    // Slot 0 should contain the last published mid_price = 100.0 + k_capacity.
    const char* slot0 = m + trading::LobPublisher::k_header_size;
    double mid;
    std::memcpy(&mid, slot0 + 24, 8);
    EXPECT_NEAR(mid, 100.0 + static_cast<double>(trading::LobPublisher::k_capacity),
                1e-6);

    ::munmap(const_cast<char*>(m), total);
    ::close(fd);
}

TEST(LobPublisher, PublishWhenClosedIsNoop) {
    trading::LobPublisher pub; // default path, never opened
    std::vector<trading::PriceLevel> bids = {{100.0, 1.0}};
    std::vector<trading::PriceLevel> asks = {{100.1, 1.0}};
    // Must not crash.
    pub.publish(trading::Exchange::BINANCE, "BTCUSDT", 1LL, 100.0, bids, asks);
}

} // namespace
