#include "core/orderbook/orderbook.hpp"
#include <gtest/gtest.h>
#include <cmath>

using namespace trading;

// Build a synthetic snapshot centered around a given best bid.
static Snapshot make_snapshot(double best_bid, double best_ask,
                               int n_levels = 5, double tick = 1.0,
                               uint64_t seq = 100) {
    Snapshot s;
    s.symbol   = "BTCUSDT";
    s.exchange = Exchange::BINANCE;
    s.sequence = seq;
    for (int i = 0; i < n_levels; ++i) {
        s.bids.push_back(PriceLevel(best_bid  - i * tick, 1.0 + i));
        s.asks.push_back(PriceLevel(best_ask  + i * tick, 1.0 + i));
    }
    return s;
}

static Delta make_delta(Side side, double price, double size, uint64_t seq) {
    Delta d;
    d.side     = side;
    d.price    = price;
    d.size     = size;
    d.sequence = seq;
    return d;
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
    Delta d60 = make_delta(Side::BID, 55000.0, 3.7, 102);  // within 20k range
    EXPECT_EQ(book.apply_delta(d50), Result::SUCCESS);
    EXPECT_EQ(book.apply_delta(d60), Result::SUCCESS);

    // Both prices must be independently retrievable
    std::vector<PriceLevel> bids, asks;
    book.get_top_levels(20000, bids, asks);

    bool found50 = false, found55 = false;
    for (const auto& l : bids) {
        if (std::fabs(l.price - 50000.0) < 0.01) { found50 = true; EXPECT_DOUBLE_EQ(l.size, 2.5); }
        if (std::fabs(l.price - 55000.0) < 0.01) { found55 = true; EXPECT_DOUBLE_EQ(l.size, 3.7); }
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
            double best = book.get_best_bid();
            // best_bid scan may return a different price if there are others,
            // so just check the specific level via get_top_levels
            std::vector<PriceLevel> bids, asks;
            book.get_top_levels(20000, bids, asks);
            bool found = false;
            for (const auto& l : bids) {
                if (std::fabs(l.price - price) < 0.01) { found = true; break; }
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
        EXPECT_GT(bids[i-1].price, bids[i].price);
    }
    // Asks: ascending
    for (size_t i = 1; i < asks.size(); ++i) {
        EXPECT_LT(asks[i-1].price, asks[i].price);
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
    EXPECT_EQ(book.get_sequence(), 101u);  // unchanged
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

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
