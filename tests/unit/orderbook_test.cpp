#include "core/orderbook/orderbook.hpp"
#include <cmath>
#include <cstdint>
#include <gtest/gtest.h>

using namespace trading;

// Build a synthetic snapshot centered around a given best bid.
static Snapshot make_snapshot(double best_bid, double best_ask, int n_levels = 5, double tick = 1.0,
                              uint64_t seq = 100) {
    Snapshot s;
    s.symbol = "BTCUSDT";
    s.exchange = Exchange::BINANCE;
    s.sequence = seq;
    for (int i = 0; i < n_levels; ++i) {
        s.bids.push_back(PriceLevel(best_bid - i * tick, 1.0 + i));
        s.asks.push_back(PriceLevel(best_ask + i * tick, 1.0 + i));
    }
    return s;
}

static Delta make_delta(Side side, double price, double size, uint64_t seq) {
    Delta d;
    d.side = side;
    d.price = price;
    d.size = size;
    d.sequence = seq;
    return d;
}

static uint32_t snapshot_checksum(const Snapshot& s) {
    auto fnv1a_bytes = [](const char* data, size_t len, uint32_t seed) {
        uint32_t hash = seed;
        for (size_t i = 0; i < len; ++i) {
            hash ^= static_cast<uint8_t>(data[i]);
            hash *= 16777619u;
        }
        return hash;
    };

    uint32_t hash = 2166136261u;
    auto hash_level = [&](const PriceLevel& level) {
        hash = fnv1a_bytes(reinterpret_cast<const char*>(&level.price), sizeof(level.price), hash);
        hash = fnv1a_bytes(reinterpret_cast<const char*>(&level.size), sizeof(level.size), hash);
    };

    for (const auto& l : s.bids)
        hash_level(l);
    for (const auto& l : s.asks)
        hash_level(l);
    return hash;
}
// ─────────────────────────────────────────────

TEST(OrderBook, ConstructionDefaults) {
    OrderBook book("BTCUSDT", Exchange::BINANCE, 1.0, 20000);
    EXPECT_FALSE(book.is_initialized());
    EXPECT_EQ(book.get_sequence(), 0u);
    EXPECT_EQ(book.get_best_bid(), 0.0);
    EXPECT_EQ(book.get_best_ask(), 0.0);
}

TEST(OrderBook, DeltaBeforeSnapshotReturnsError) {
    OrderBook book("BTCUSDT", Exchange::BINANCE, 1.0, 20000);
    Delta d = make_delta(Side::BID, 50000.0, 1.0, 101);
    EXPECT_EQ(book.apply_delta(d), Result::ERROR_BOOK_CORRUPTED);
}

TEST(OrderBook, ApplySnapshotSetsBasePrice) {
    OrderBook book("BTCUSDT", Exchange::BINANCE, 1.0, 20000);
    Snapshot s = make_snapshot(50000.0, 50001.0, 5, 1.0, 100);

    EXPECT_EQ(book.apply_snapshot(s), Result::SUCCESS);
    EXPECT_TRUE(book.is_initialized());
    EXPECT_EQ(book.get_sequence(), 100u);

    // Base price should center grid: base = best_bid - (levels/2)*tick
    double expected_base = 50000.0 - (20000 / 2) * 1.0;
    EXPECT_DOUBLE_EQ(book.base_price(), expected_base);
}

TEST(OrderBook, PriceGridNoCollisions) {
    // The old code mapped $50,000 and $60,000 both to index 0 via % MAX_LEVELS.
    // The new code must give distinct indices.
    OrderBook book("BTCUSDT", Exchange::BINANCE, 1.0, 20000);

    // Snapshot centered at $50,000 → base = $40,000
    Snapshot s = make_snapshot(50000.0, 50001.0, 1, 1.0, 100);
    book.apply_snapshot(s);

    // Apply two deltas at different prices
    Delta d50 = make_delta(Side::BID, 50000.0, 2.5, 101);
    Delta d60 = make_delta(Side::BID, 55000.0, 3.7, 102); // within 20k range
    EXPECT_EQ(book.apply_delta(d50), Result::SUCCESS);
    EXPECT_EQ(book.apply_delta(d60), Result::SUCCESS);

    // Both prices must be independently retrievable
    std::vector<PriceLevel> bids, asks;
    book.get_top_levels(20000, bids, asks);

    bool found50 = false, found55 = false;
    for (const auto& l : bids) {
        if (std::fabs(l.price - 50000.0) < 0.01) {
            found50 = true;
            EXPECT_DOUBLE_EQ(l.size, 2.5);
        }
        if (std::fabs(l.price - 55000.0) < 0.01) {
            found55 = true;
            EXPECT_DOUBLE_EQ(l.size, 3.7);
        }
    }
    EXPECT_TRUE(found50) << "50000 level not found";
    EXPECT_TRUE(found55) << "55000 level not found";
}

TEST(OrderBook, PriceToIndexRoundTrip) {
    OrderBook book("BTCUSDT", Exchange::BINANCE, 1.0, 20000);
    Snapshot s = make_snapshot(50000.0, 50001.0, 1, 1.0, 100);
    book.apply_snapshot(s);

    // Prices within grid should round-trip through the index mapping.
    for (double price : {49000.0, 50000.0, 50500.0, 51234.0, 58000.0}) {
        Delta d = make_delta(Side::BID, price, 1.0, book.get_sequence() + 1);
        if (book.apply_delta(d) == Result::SUCCESS) {
            // best_bid scan may return a different price if there are others,
            // so just check the specific level via get_top_levels
            std::vector<PriceLevel> bids, asks;
            book.get_top_levels(20000, bids, asks);
            bool found = false;
            for (const auto& l : bids) {
                if (std::fabs(l.price - price) < 0.01) {
                    found = true;
                    break;
                }
            }
            EXPECT_TRUE(found) << "round-trip failed for price " << price;
        }
    }
}

TEST(OrderBook, ApplyDeltaBid) {
    OrderBook book("BTCUSDT", Exchange::BINANCE, 1.0, 20000);
    book.apply_snapshot(make_snapshot(50000.0, 50001.0, 1, 1.0, 100));

    Delta d = make_delta(Side::BID, 50000.0, 9.9, 101);
    EXPECT_EQ(book.apply_delta(d), Result::SUCCESS);
    EXPECT_DOUBLE_EQ(book.get_best_bid(), 50000.0);
    EXPECT_EQ(book.get_sequence(), 101u);
}

TEST(OrderBook, ApplyDeltaAsk) {
    OrderBook book("BTCUSDT", Exchange::BINANCE, 1.0, 20000);
    book.apply_snapshot(make_snapshot(50000.0, 50001.0, 1, 1.0, 100));

    Delta d = make_delta(Side::ASK, 50001.0, 7.7, 101);
    EXPECT_EQ(book.apply_delta(d), Result::SUCCESS);
    EXPECT_DOUBLE_EQ(book.get_best_ask(), 50001.0);
}

TEST(OrderBook, ApplyDeltaSizeZeroRemovesLevel) {
    OrderBook book("BTCUSDT", Exchange::BINANCE, 1.0, 20000);
    book.apply_snapshot(make_snapshot(50000.0, 50001.0, 1, 1.0, 100));

    // Remove the best bid
    Delta d = make_delta(Side::BID, 50000.0, 0.0, 101);
    EXPECT_EQ(book.apply_delta(d), Result::SUCCESS);
    // Next best bid is the snapshot level one tick below
    EXPECT_NE(book.get_best_bid(), 50000.0);
}

TEST(OrderBook, OutOfRangePriceReturnsError) {
    OrderBook book("BTCUSDT", Exchange::BINANCE, 1.0, 20000);
    book.apply_snapshot(make_snapshot(50000.0, 50001.0, 1, 1.0, 100));

    // base = 40000, range = 20000 → max price = 59999
    // A price of 70000 is well out of range
    Delta d = make_delta(Side::BID, 70000.0, 1.0, 101);
    EXPECT_EQ(book.apply_delta(d), Result::ERROR_INVALID_PRICE);
}

TEST(OrderBook, RecentersAfterRepeatedOutOfRangeDeltas) {
    OrderBook book("BTCUSDT", Exchange::BINANCE, 1.0, 20000);
    book.apply_snapshot(make_snapshot(50000.0, 50001.0, 2, 1.0, 100));

    const double initial_base = book.base_price();

    // Four consecutive out-of-grid deltas should trigger adaptive recentering.
    EXPECT_EQ(book.apply_delta(make_delta(Side::BID, 65000.0, 1.0, 101)),
              Result::ERROR_INVALID_PRICE);
    EXPECT_EQ(book.apply_delta(make_delta(Side::BID, 65000.0, 1.1, 102)),
              Result::ERROR_INVALID_PRICE);
    EXPECT_EQ(book.apply_delta(make_delta(Side::BID, 65000.0, 1.2, 103)),
              Result::ERROR_INVALID_PRICE);
    EXPECT_EQ(book.apply_delta(make_delta(Side::BID, 65000.0, 9.5, 104)), Result::SUCCESS);

    EXPECT_NE(book.base_price(), initial_base);
    EXPECT_DOUBLE_EQ(book.get_best_bid(), 65000.0);
    EXPECT_EQ(book.get_sequence(), 104u);
}

TEST(OrderBook, RecentersImmediatelyOnLargeBreach) {
    OrderBook book("BTCUSDT", Exchange::BINANCE, 1.0, 20000);
    book.apply_snapshot(make_snapshot(50000.0, 50001.0, 2, 1.0, 100));

    const double initial_base = book.base_price();

    // Extreme overshoot should force immediate recenter instead of repeated rejection.
    EXPECT_EQ(book.apply_delta(make_delta(Side::ASK, 85000.0, 5.0, 101)), Result::SUCCESS);
    EXPECT_NE(book.base_price(), initial_base);
    EXPECT_DOUBLE_EQ(book.get_best_ask(), 85000.0);
}

TEST(OrderBook, RecenterPreservesInRangeLiquidity) {
    OrderBook book("BTCUSDT", Exchange::BINANCE, 1.0, 20000);
    book.apply_snapshot(make_snapshot(50000.0, 50001.0, 2, 1.0, 100));

    // Seed liquidity that should still be in range after recentering.
    EXPECT_EQ(book.apply_delta(make_delta(Side::BID, 52000.0, 2.5, 101)), Result::SUCCESS);
    EXPECT_EQ(book.apply_delta(make_delta(Side::ASK, 52001.0, 3.5, 102)), Result::SUCCESS);

    EXPECT_EQ(book.apply_delta(make_delta(Side::ASK, 62000.0, 6.0, 103)),
              Result::ERROR_INVALID_PRICE);
    EXPECT_EQ(book.apply_delta(make_delta(Side::ASK, 62000.0, 6.1, 104)),
              Result::ERROR_INVALID_PRICE);
    EXPECT_EQ(book.apply_delta(make_delta(Side::ASK, 62000.0, 6.2, 105)),
              Result::ERROR_INVALID_PRICE);
    EXPECT_EQ(book.apply_delta(make_delta(Side::ASK, 62000.0, 6.3, 106)), Result::SUCCESS);

    std::vector<PriceLevel> bids, asks;
    book.get_top_levels(20000, bids, asks);

    bool found_bid = false;
    bool found_ask = false;
    for (const auto& l : bids) {
        if (std::fabs(l.price - 52000.0) < 0.01) {
            found_bid = true;
            EXPECT_DOUBLE_EQ(l.size, 2.5);
        }
    }
    for (const auto& l : asks) {
        if (std::fabs(l.price - 52001.0) < 0.01) {
            found_ask = true;
            EXPECT_DOUBLE_EQ(l.size, 3.5);
        }
    }

    EXPECT_TRUE(found_bid);
    EXPECT_TRUE(found_ask);
}

TEST(OrderBook, GetBestBidAsk) {
    OrderBook book("BTCUSDT", Exchange::BINANCE, 1.0, 20000);
    Snapshot s = make_snapshot(50000.0, 50001.0, 3, 1.0, 100);
    book.apply_snapshot(s);

    EXPECT_DOUBLE_EQ(book.get_best_bid(), 50000.0);
    EXPECT_DOUBLE_EQ(book.get_best_ask(), 50001.0);
    EXPECT_DOUBLE_EQ(book.get_mid_price(), 50000.5);
    EXPECT_DOUBLE_EQ(book.get_spread(), 1.0);
}

TEST(OrderBook, GetTopLevelsSorted) {
    OrderBook book("BTCUSDT", Exchange::BINANCE, 1.0, 20000);
    // 5 bid levels at 50000, 49999, 49998, 49997, 49996
    // 5 ask levels at 50001, 50002, 50003, 50004, 50005
    book.apply_snapshot(make_snapshot(50000.0, 50001.0, 5, 1.0, 100));

    std::vector<PriceLevel> bids, asks;
    book.get_top_levels(5, bids, asks);

    ASSERT_EQ(bids.size(), 5u);
    ASSERT_EQ(asks.size(), 5u);

    // Bids: descending
    for (size_t i = 1; i < bids.size(); ++i) {
        EXPECT_GT(bids[i - 1].price, bids[i].price);
    }
    // Asks: ascending
    for (size_t i = 1; i < asks.size(); ++i) {
        EXPECT_LT(asks[i - 1].price, asks[i].price);
    }

    EXPECT_DOUBLE_EQ(bids[0].price, 50000.0);
    EXPECT_DOUBLE_EQ(asks[0].price, 50001.0);
}

TEST(OrderBook, OldDeltaSequenceIgnored) {
    OrderBook book("BTCUSDT", Exchange::BINANCE, 1.0, 20000);
    book.apply_snapshot(make_snapshot(50000.0, 50001.0, 1, 1.0, 100));

    // Apply a valid delta first
    book.apply_delta(make_delta(Side::BID, 49999.0, 5.0, 101));
    EXPECT_EQ(book.get_sequence(), 101u);

    // Stale delta (seq=50, older than snapshot seq=100) must be ignored
    auto result = book.apply_delta(make_delta(Side::BID, 49999.0, 99.0, 50));
    EXPECT_EQ(result, Result::SUCCESS);
    EXPECT_EQ(book.get_sequence(), 101u); // unchanged
}

TEST(OrderBook, ReSnapshotRecentersGrid) {
    OrderBook book("BTCUSDT", Exchange::BINANCE, 1.0, 20000);
    book.apply_snapshot(make_snapshot(50000.0, 50001.0, 1, 1.0, 100));
    double base1 = book.base_price();

    // Re-snapshot at a new price
    book.apply_snapshot(make_snapshot(80000.0, 80001.0, 1, 1.0, 200));
    double base2 = book.base_price();

    EXPECT_NE(base1, base2);
    EXPECT_DOUBLE_EQ(base2, 80000.0 - (20000 / 2) * 1.0);
    EXPECT_DOUBLE_EQ(book.get_best_bid(), 80000.0);
    EXPECT_EQ(book.get_sequence(), 200u);
}

TEST(OrderBook, RejectsPathologicallyWideSpreadSnapshot) {
    OrderBook book("BTCUSDT", Exchange::BINANCE, 1.0, 20000);
    Snapshot s = make_snapshot(50000.0, 56001.0, 3, 1.0, 100);

    EXPECT_EQ(book.apply_snapshot(s), Result::ERROR_INVALID_PRICE);
    EXPECT_FALSE(book.is_initialized());
}

TEST(OrderBook, AcceptsSnapshotWithValidChecksum) {
    OrderBook book("BTCUSDT", Exchange::BINANCE, 1.0, 20000);
    Snapshot s = make_snapshot(50000.0, 50001.0, 3, 1.0, 100);
    s.checksum = snapshot_checksum(s);
    s.checksum_present = true;

    EXPECT_EQ(book.apply_snapshot(s), Result::SUCCESS);
}

TEST(OrderBook, RejectsSnapshotWithInvalidChecksum) {
    OrderBook book("BTCUSDT", Exchange::BINANCE, 1.0, 20000);
    Snapshot s = make_snapshot(50000.0, 50001.0, 3, 1.0, 100);
    s.checksum = 42;
    s.checksum_present = true;

    EXPECT_EQ(book.apply_snapshot(s), Result::ERROR_BOOK_CORRUPTED);
    EXPECT_FALSE(book.is_initialized());
}

// Regression: base_price_ can be exactly 0.0 for low-priced assets.
// E.g. ETH at $1000, tick=0.10, max_levels=20000 → base = 1000 - 10000*0.10 = 0.
// The old code used (base_price_ != 0.0) as the initialized sentinel, giving a false
// negative that permanently blocked all delta processing.
TEST(OrderBook, IsInitializedWhenBasePriceIsZero) {
    // tick=0.10, max_levels=20000 → half_range = 10000 * 0.10 = 1000
    // best_bid=1000 → base = 1000 - 1000 = 0.0  (the problematic case)
    OrderBook book("ETHUSD", Exchange::KRAKEN, 0.10, 20000);
    Snapshot s;
    s.symbol   = "ETHUSD";
    s.exchange = Exchange::KRAKEN;
    s.sequence = 1;
    s.bids.push_back(PriceLevel(1000.0, 5.0));
    s.asks.push_back(PriceLevel(1000.1, 5.0));

    EXPECT_EQ(book.apply_snapshot(s), Result::SUCCESS);
    EXPECT_TRUE(book.is_initialized());

    // Deltas must also be accepted — base_price_ == 0.0 must not block them.
    Delta d = make_delta(Side::BID, 1000.0, 3.0, 2);
    EXPECT_EQ(book.apply_delta(d), Result::SUCCESS);
    EXPECT_DOUBLE_EQ(book.get_best_bid(), 1000.0);
}

TEST(OrderBook, NegativeSizeDeltaRejected) {
    OrderBook book("BTCUSDT", Exchange::BINANCE, 1.0, 20000);
    book.apply_snapshot(make_snapshot(50000.0, 50001.0, 1, 1.0, 100));

    Delta d = make_delta(Side::BID, 50000.0, -1.0, 101);
    EXPECT_EQ(book.apply_delta(d), Result::ERROR_INVALID_SIZE);
    // Sequence must not advance on a rejected delta.
    EXPECT_EQ(book.get_sequence(), 100u);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
