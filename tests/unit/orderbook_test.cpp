#include "core/feeds/common/tick_size.hpp"
#include "core/orderbook/orderbook.hpp"
#include <cmath>
#include <cstdint>
#include <gtest/gtest.h>

using namespace trading;

TEST(TickFromString, PowersOfTen) {
    EXPECT_DOUBLE_EQ(tick_from_string("0.01000000"), std::pow(10.0, -2));
    EXPECT_DOUBLE_EQ(tick_from_string("0.001"),      std::pow(10.0, -3));
    EXPECT_DOUBLE_EQ(tick_from_string("0.10000000"), std::pow(10.0, -1));
    EXPECT_DOUBLE_EQ(tick_from_string("1.00000000"), 1.0);
    EXPECT_DOUBLE_EQ(tick_from_string("0.1"),        std::pow(10.0, -1));
}

TEST(TickFromString, NonPowerFallsBackToStod) {
    EXPECT_DOUBLE_EQ(tick_from_string("0.25"), 0.25);
    EXPECT_DOUBLE_EQ(tick_from_string("5"),    5.0);
}

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

// ETH at $1001, tick=0.10 → base = 1.0 > 0; initialized sentinel works correctly.
TEST(OrderBook, IsInitializedWithSmallPositiveBase) {
    OrderBook book("ETHUSD", Exchange::KRAKEN, 0.10, 20000);
    Snapshot s;
    s.symbol   = "ETHUSD";
    s.exchange = Exchange::KRAKEN;
    s.sequence = 1;
    s.bids.push_back(PriceLevel(1001.0, 5.0));
    s.asks.push_back(PriceLevel(1001.1, 5.0));

    EXPECT_EQ(book.apply_snapshot(s), Result::SUCCESS);
    EXPECT_TRUE(book.is_initialized());
    EXPECT_GT(book.base_price(), 0.0);

    // Deltas must be accepted.
    Delta d = make_delta(Side::BID, 1001.0, 3.0, 2);
    EXPECT_EQ(book.apply_delta(d), Result::SUCCESS);
    EXPECT_DOUBLE_EQ(book.get_best_bid(), 1001.0);
}

TEST(OrderBook, NegativeSizeDeltaRejected) {
    OrderBook book("BTCUSDT", Exchange::BINANCE, 1.0, 20000);
    book.apply_snapshot(make_snapshot(50000.0, 50001.0, 1, 1.0, 100));

    Delta d = make_delta(Side::BID, 50000.0, -1.0, 101);
    EXPECT_EQ(book.apply_delta(d), Result::ERROR_INVALID_SIZE);
    // Sequence must not advance on a rejected delta.
    EXPECT_EQ(book.get_sequence(), 100u);
}

// Snapshot + delta round-trip for all four exchanges.
// The exchange field is metadata only; verifies it doesn't affect book logic.
TEST(OrderBook, CompatibleWithAllFourExchanges) {
    struct Case {
        Exchange ex;
        const char* symbol;
        double bid;
        double ask;
        double tick;
    };
    const Case cases[] = {
        {Exchange::BINANCE, "BTCUSDT", 50000.0, 50001.0, 1.0},
        {Exchange::OKX, "BTC-USDT", 50000.0, 50001.0, 1.0},
        {Exchange::COINBASE, "BTC-USD", 50000.0, 50001.0, 1.0},
        {Exchange::KRAKEN, "XBT/USD", 50000.0, 50001.0, 1.0},
    };

    for (const auto& c : cases) {
        OrderBook book(c.symbol, c.ex, c.tick, 20000);
        Snapshot s;
        s.symbol   = c.symbol;
        s.exchange = c.ex;
        s.sequence = 100;
        s.bids.push_back(PriceLevel(c.bid, 1.0));
        s.asks.push_back(PriceLevel(c.ask, 1.0));

        EXPECT_EQ(book.apply_snapshot(s), Result::SUCCESS) << "exchange=" << (int)c.ex;
        EXPECT_TRUE(book.is_initialized())                 << "exchange=" << (int)c.ex;
        EXPECT_EQ(book.exchange(), c.ex)                   << "exchange=" << (int)c.ex;

        Delta d = make_delta(Side::BID, c.bid, 2.5, 101);
        EXPECT_EQ(book.apply_delta(d), Result::SUCCESS)    << "exchange=" << (int)c.ex;
        EXPECT_DOUBLE_EQ(book.get_best_bid(), c.bid)       << "exchange=" << (int)c.ex;
    }
}

// SOL at $20, tick=0.01, max_levels=20000 → base = 20 - 10000*0.01 = -80, rejected.
TEST(OrderBook, NegativeBasePriceSnapshotRejected) {
    OrderBook book("SOLUSD", Exchange::COINBASE, 0.01, 20000);
    Snapshot s;
    s.symbol   = "SOLUSD";
    s.exchange = Exchange::COINBASE;
    s.sequence = 1;
    s.bids.push_back(PriceLevel(20.00, 10.0));
    s.asks.push_back(PriceLevel(20.01, 8.0));

    // base = 20 - 10000*0.01 = -80  →  must be rejected
    EXPECT_EQ(book.apply_snapshot(s), Result::ERROR_INVALID_PRICE);
    EXPECT_FALSE(book.is_initialized());
    EXPECT_EQ(book.get_best_bid(), 0.0);
    EXPECT_EQ(book.get_best_ask(), 0.0);
}

// SOL at $20, tick=0.001, max_levels=20000 → base = 10 > 0, accepted.
TEST(OrderBook, SOLCorrectTickSizePositiveBase) {
    OrderBook book("SOLUSD", Exchange::COINBASE, 0.001, 20000);
    Snapshot s;
    s.symbol   = "SOLUSD";
    s.exchange = Exchange::COINBASE;
    s.sequence = 1;
    s.bids.push_back(PriceLevel(20.000, 10.0));
    s.asks.push_back(PriceLevel(20.001, 8.0));

    EXPECT_EQ(book.apply_snapshot(s), Result::SUCCESS);
    EXPECT_TRUE(book.is_initialized());
    EXPECT_GT(book.base_price(), 0.0);
    EXPECT_DOUBLE_EQ(book.get_best_bid(), 20.000);
    EXPECT_DOUBLE_EQ(book.get_best_ask(), 20.001);

    // Deltas must also be accepted.
    Delta d = make_delta(Side::BID, 19.999, 3.0, 2);
    EXPECT_EQ(book.apply_delta(d), Result::SUCCESS);

    std::vector<PriceLevel> bids, asks;
    book.get_top_levels(5, bids, asks);
    ASSERT_GE(bids.size(), 2u);
    EXPECT_DOUBLE_EQ(bids[0].price, 20.000);
    EXPECT_DOUBLE_EQ(bids[1].price, 19.999);
}

// SOL at $140, tick=0.01 (exchange-sourced) → base = 40 > 0, accepted.
TEST(OrderBook, SOLAt140CorrectTickPositiveBase) {
    OrderBook book("SOLUSDT", Exchange::BINANCE, 0.01, 20000);
    Snapshot s;
    s.symbol   = "SOLUSDT";
    s.exchange = Exchange::BINANCE;
    s.sequence = 1;
    s.bids.push_back(PriceLevel(140.00, 500.0));
    s.asks.push_back(PriceLevel(140.01, 450.0));

    EXPECT_EQ(book.apply_snapshot(s), Result::SUCCESS);
    EXPECT_TRUE(book.is_initialized());
    EXPECT_DOUBLE_EQ(book.base_price(), 140.00 - 10000 * 0.01); // 40.0
    EXPECT_GT(book.base_price(), 0.0);
    EXPECT_DOUBLE_EQ(book.get_best_bid(), 140.00);
    EXPECT_DOUBLE_EQ(book.get_best_ask(), 140.01);
}

// SOL at $140, tick=0.1 (oversized, wrong precision) → base = -860, rejected.
TEST(OrderBook, SOLLargeTickNegativeBaseRejected) {
    OrderBook book("SOLUSDT", Exchange::BINANCE, 0.1, 20000);
    Snapshot s;
    s.symbol   = "SOLUSDT";
    s.exchange = Exchange::BINANCE;
    s.sequence = 1;
    s.bids.push_back(PriceLevel(140.0, 500.0));
    s.asks.push_back(PriceLevel(140.1, 450.0));

    // base = 140 - 10000*0.1 = -860  →  must be rejected
    EXPECT_EQ(book.apply_snapshot(s), Result::ERROR_INVALID_PRICE);
    EXPECT_FALSE(book.is_initialized());
}

// BTC at $69000, tick=1.0 → base = 59000 > 0, accepted.
TEST(OrderBook, BTCTypicalTickPositiveBase) {
    OrderBook book("BTCUSDT", Exchange::BINANCE, 1.0, 20000);
    Snapshot s;
    s.symbol   = "BTCUSDT";
    s.exchange = Exchange::BINANCE;
    s.sequence = 1;
    s.bids.push_back(PriceLevel(69000.0, 1.0));
    s.asks.push_back(PriceLevel(69001.0, 1.0));

    EXPECT_EQ(book.apply_snapshot(s), Result::SUCCESS);
    EXPECT_TRUE(book.is_initialized());
    EXPECT_DOUBLE_EQ(book.base_price(), 69000.0 - 10000 * 1.0); // 59000.0
    EXPECT_GT(book.base_price(), 0.0);
}

// ETH at $1000, tick=0.10, max_levels=20000 → base = 0.0, rejected (log(0) undefined).
TEST(OrderBook, ZeroBasePriceRejected) {
    OrderBook book("ETHUSD", Exchange::KRAKEN, 0.10, 20000);
    Snapshot s;
    s.symbol   = "ETHUSD";
    s.exchange = Exchange::KRAKEN;
    s.sequence = 1;
    s.bids.push_back(PriceLevel(1000.0, 5.0));
    s.asks.push_back(PriceLevel(1000.1, 5.0));

    EXPECT_EQ(book.apply_snapshot(s), Result::ERROR_INVALID_PRICE);
    EXPECT_FALSE(book.is_initialized());
}

// Price 1 tick below the grid base must be rejected, not silently mapped to index 0.
TEST(OrderBook, PriceOneTickBelowBaseRejected) {
    OrderBook book("BTCUSDT", Exchange::BINANCE, 1.0, 20000);
    book.apply_snapshot(make_snapshot(50000.0, 50001.0, 1, 1.0, 100));

    // base = 50000 - 10000 = 40000.
    // A price of 39999.0 is exactly 1 tick below the base → must be rejected.
    Delta d = make_delta(Side::BID, 39999.0, 1.0, 101);
    EXPECT_EQ(book.apply_delta(d), Result::ERROR_INVALID_PRICE);
}

TEST(OrderBook, PriceAtBaseAccepted) {
    OrderBook book("BTCUSDT", Exchange::BINANCE, 1.0, 20000);
    book.apply_snapshot(make_snapshot(50000.0, 50001.0, 1, 1.0, 100));

    // base = 40000.  A bid at exactly base maps to index 0.
    double base = book.base_price();
    Delta d = make_delta(Side::BID, base, 2.0, 101);
    EXPECT_EQ(book.apply_delta(d), Result::SUCCESS);
}

TEST(OrderBook, AdjacentTicksDontCollide) {
    OrderBook book("BTCUSDT", Exchange::BINANCE, 1.0, 20000);
    book.apply_snapshot(make_snapshot(50000.0, 50001.0, 1, 1.0, 100));

    Delta d1 = make_delta(Side::BID, 48000.0, 1.1, 101);
    Delta d2 = make_delta(Side::BID, 48001.0, 2.2, 102);
    EXPECT_EQ(book.apply_delta(d1), Result::SUCCESS);
    EXPECT_EQ(book.apply_delta(d2), Result::SUCCESS);

    std::vector<PriceLevel> bids, asks;
    book.get_top_levels(20000, bids, asks);

    bool found48000 = false, found48001 = false;
    for (const auto& l : bids) {
        if (std::fabs(l.price - 48000.0) < 0.01) {
            found48000 = true;
            EXPECT_DOUBLE_EQ(l.size, 1.1);
        }
        if (std::fabs(l.price - 48001.0) < 0.01) {
            found48001 = true;
            EXPECT_DOUBLE_EQ(l.size, 2.2);
        }
    }
    EXPECT_TRUE(found48000) << "48000 level missing";
    EXPECT_TRUE(found48001) << "48001 level missing";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
