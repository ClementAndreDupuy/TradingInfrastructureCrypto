#include "core/engine/algo_config_loader.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <fstream>
#include <filesystem>

using namespace trading;

TEST(AlgoConfigLoaderTest, LoadsBinanceFuturesRuntimeConfig) {
    const auto path = std::filesystem::temp_directory_path() / "algo_config_loader_test_engine.yaml";
    std::ofstream out(path);
    ASSERT_TRUE(out.is_open());
    out << "strategy_mode: futures_only\n";
    out << "binance_futures_enabled: true\n";
    out << "binance_futures_rest_url: https://fapi.binance.com\n";
    out << "binance_futures_recv_window_ms: 5000\n";
    out << "binance_futures_hedge_mode: true\n";
    out << "binance_futures_default_leverage_cap: 8.0\n";
    out << "binance_futures_leverage_cap_BTCUSDT: 12.0\n";
    out << "binance_futures_leverage_cap_ETHUSDT: 10.0\n";
    out.close();

    EngineConfig cfg;
    ASSERT_TRUE(AlgoConfigLoader::load_engine(path.string(), cfg));
    EXPECT_EQ(cfg.strategy_mode, StrategyMode::FUTURES_ONLY);
    EXPECT_TRUE(cfg.binance_futures.enabled);
    EXPECT_EQ(cfg.binance_futures.rest_url, "https://fapi.binance.com");
    EXPECT_EQ(cfg.binance_futures.recv_window_ms, 5000U);
    EXPECT_TRUE(cfg.binance_futures.hedge_mode);
    EXPECT_DOUBLE_EQ(cfg.binance_futures.default_leverage_cap, 8.0);
    ASSERT_EQ(cfg.binance_futures.leverage_caps.size(), 2U);
    std::sort(cfg.binance_futures.leverage_caps.begin(), cfg.binance_futures.leverage_caps.end(),
              [](const FuturesLeverageCap &lhs, const FuturesLeverageCap &rhs) {
                  return lhs.symbol < rhs.symbol;
              });
    EXPECT_EQ(cfg.binance_futures.leverage_caps[0].symbol, "BTCUSDT");
    EXPECT_DOUBLE_EQ(cfg.binance_futures.leverage_caps[0].max_leverage, 12.0);
    EXPECT_EQ(cfg.binance_futures.leverage_caps[1].symbol, "ETHUSDT");
    EXPECT_DOUBLE_EQ(cfg.binance_futures.leverage_caps[1].max_leverage, 10.0);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}
