#include "core/feeds/binance/binance_feed_handler.hpp"
#include "core/orderbook/orderbook.hpp"
#include "core/risk/circuit_breaker.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>

namespace trading {
namespace {

template <typename Fn> double avg_ns(Fn&& fn, int iters) {
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i)
        fn(i);
    auto t1 = std::chrono::steady_clock::now();
    const double total_ns =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    return total_ns / static_cast<double>(iters);
}

TEST(LatencyBudgetTest, OrderBookDeltaUnderOneMicrosecond) {
    OrderBook book("BTCUSDT", Exchange::BINANCE, 1.0, 20000);
    Snapshot snap;
    snap.symbol = "BTCUSDT";
    snap.exchange = Exchange::BINANCE;
    snap.sequence = 1;
    snap.bids.push_back({50000.0, 1.0});
    snap.asks.push_back({50001.0, 1.0});
    ASSERT_EQ(book.apply_snapshot(snap), Result::SUCCESS);

    const double ns = avg_ns(
        [&](int i) {
            Delta d;
            d.side = (i & 1) ? Side::BID : Side::ASK;
            d.price = 50000.0 + (i % 5);
            d.size = 1.0;
            d.sequence = static_cast<uint64_t>(2 + i);
            book.apply_delta(d);
        },
        200000);

    EXPECT_LT(ns, 1000.0) << "orderbook delta avg ns=" << ns;
}

TEST(LatencyBudgetTest, RiskCheckUnderOneMicrosecond) {
    KillSwitch ks;
    CircuitBreakerConfig cfg;
    cfg.max_orders_per_second = 1'000'000;
    cfg.max_orders_per_minute = 2'000'000;
    CircuitBreaker cb(cfg, ks);

    const double ns = avg_ns([&](int) { cb.check_order_rate(); }, 200000);

    EXPECT_LT(ns, 1000.0) << "risk check avg ns=" << ns;
}

TEST(LatencyBudgetTest, FeedMessageProcessingUnderTenMicroseconds) {
    BinanceFeedHandler h("BTCUSDT");
    const std::string msg =
        R"({"e":"depthUpdate","s":"BTCUSDT","U":1,"u":1,"b":[["50000.0","1.0"]],"a":[["50001.0","1.0"]]})";

    const double ns = avg_ns([&](int) { h.process_message(msg); }, 100000);

    EXPECT_LT(ns, 10000.0) << "feed process avg ns=" << ns;
}

} // namespace
} // namespace trading
