// Perpetual Futures Arbitrage — Main Entry Point
//
// Wires together:
//   - Binance Perp feed  (fstream.binance.com) → binance BookManager
//   - Kraken Futures feed (futures.kraken.com)  → kraken  BookManager
//   - ArbRiskManager  (pre-trade checks + kill switch)
//   - OrderManager    (order lifecycle tracking)
//   - ShadowConnectors OR real connectors (controlled by --shadow flag)
//   - PerpArbStrategy (taker arb + market maker logic)
//
// Usage:
//   ./perp_arb [--shadow]           # shadow mode (default: live)
//   ./perp_arb --shadow --qty 0.001 # shadow with 0.001 BTC per leg
//
// Environment variables required for live mode:
//   BINANCE_API_KEY, BINANCE_API_SECRET
//   KRAKEN_API_KEY,  KRAKEN_API_SECRET

#include "../feeds/binance/binance_feed_handler.hpp"
#include "../feeds/kraken/kraken_futures_feed_handler.hpp"
#include "../feeds/book_manager.hpp"
#include "../execution/order_manager.hpp"
#include "../execution/binance_connector.hpp"
#include "../execution/kraken_connector.hpp"
#include "../risk/arb_risk_manager.hpp"
#include "../shadow/shadow_engine.hpp"
#include "perp_arb_strategy.hpp"

#include <csignal>
#include <cstring>
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <string>

using namespace trading;

// ── Global shutdown flag ──────────────────────────────────────────────────────

static std::atomic<bool> g_running{true};

static void signal_handler(int) {
    g_running.store(false, std::memory_order_release);
}

// ── Minimal WebSocket event loop stub ────────────────────────────────────────
// In production this would use libwebsockets / Boost.Asio.
// The feed handlers expose process_message() for integration with any event loop.
// This stub demonstrates the wiring; replace with your WS library of choice.

struct WsSession {
    std::string url;
    std::string subscribe_msg;
    std::function<void(const std::string&)> on_message;
};

// ── Argument parsing ──────────────────────────────────────────────────────────

struct AppConfig {
    bool   shadow_mode = true;   // default: shadow (safe)
    double trade_qty   = 0.001;
    double mm_spread   = 6.0;
    double taker_spread = 12.0;
};

static AppConfig parse_args(int argc, char** argv) {
    AppConfig cfg;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--live") == 0)       cfg.shadow_mode = false;
        if (std::strcmp(argv[i], "--shadow") == 0)     cfg.shadow_mode = true;
        if (std::strcmp(argv[i], "--qty") == 0 && i+1 < argc)
            cfg.trade_qty = std::stod(argv[++i]);
        if (std::strcmp(argv[i], "--mm-spread") == 0 && i+1 < argc)
            cfg.mm_spread = std::stod(argv[++i]);
        if (std::strcmp(argv[i], "--taker-spread") == 0 && i+1 < argc)
            cfg.taker_spread = std::stod(argv[++i]);
    }
    return cfg;
}

// ── Monitoring thread ─────────────────────────────────────────────────────────

static void monitor_loop(const PerpArbStrategy& strategy,
                         const ArbRiskManager&  risk,
                         const ShadowEngine*    shadow) {
    using namespace std::chrono_literals;
    while (g_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(10s);
        LOG_INFO("=== Status ===",
                 "realized_pnl",    strategy.realized_pnl(),
                 "total_trades",    strategy.total_trades(),
                 "total_rej",       strategy.total_rej(),
                 "portfolio_pnl",   risk.get_portfolio_pnl(),
                 "open_legs",       risk.get_open_leg_count(),
                 "kill_switch",     risk.is_kill_switch_active() ? "ACTIVE" : "ok",
                 "circuit_breaker", risk.is_circuit_breaker_active() ? "TRIPPED" : "ok");

        if (shadow) {
            LOG_INFO("Shadow P&L",
                     "net_pnl",     shadow->net_pnl(),
                     "total_fills", shadow->total_fills());
        }
    }
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    AppConfig app = parse_args(argc, argv);

    LOG_INFO("Perp Arb starting",
             "mode", app.shadow_mode ? "SHADOW" : "LIVE",
             "qty",  app.trade_qty);

    // ── Book Managers ────────────────────────────────────────────────────────
    // Binance Perp: tick_size = $1 for BTC, max_levels = 20000 → ±$10k range
    // Kraken Futures: same sizing (XBT/USD is equivalent market)
    BookManager binance_book("BTCUSDT",   Exchange::BINANCE, 1.0, 20000);
    BookManager kraken_book("PI_XBTUSD", Exchange::KRAKEN,  1.0, 20000);

    // ── Feed Handlers ────────────────────────────────────────────────────────
    // Binance Perp uses fapi / fstream endpoints (not api / stream)
    BinanceFeedHandler binance_feed(
        "BTCUSDT",
        BinanceFeedHandler::get_api_key_from_env(),
        BinanceFeedHandler::get_api_secret_from_env(),
        "https://fapi.binance.com",
        "wss://fstream.binance.com/ws"
    );

    // Kraken Futures uses the dedicated futures WebSocket
    KrakenFuturesFeedHandler kraken_feed(
        "PI_XBTUSD",
        KrakenFeedHandler::get_api_key_from_env(),
        KrakenFeedHandler::get_api_secret_from_env()
    );

    // Wire feeds → books
    binance_feed.set_snapshot_callback(binance_book.snapshot_handler());
    binance_feed.set_delta_callback(binance_book.delta_handler());
    kraken_feed.set_snapshot_callback(kraken_book.snapshot_handler());
    kraken_feed.set_delta_callback(kraken_book.delta_handler());

    // ── Risk Manager ─────────────────────────────────────────────────────────
    ArbRiskConfig risk_cfg;
    risk_cfg.max_abs_position_per_symbol = 0.1;     // 0.1 BTC per exchange in dev
    risk_cfg.max_notional_per_symbol     = 10000.0;
    risk_cfg.max_portfolio_notional      = 20000.0;
    risk_cfg.max_drawdown_usd            = -500.0;
    risk_cfg.min_spread_bps              = app.taker_spread;
    risk_cfg.min_profit_usd              = 0.10;
    risk_cfg.taker_fee_bps[static_cast<int>(Exchange::BINANCE)] = 5.0;
    risk_cfg.taker_fee_bps[static_cast<int>(Exchange::KRAKEN)]  = 5.0;
    ArbRiskManager risk(risk_cfg);

    // ── Order Manager ────────────────────────────────────────────────────────
    OrderManager order_mgr;

    // ── Strategy Config ──────────────────────────────────────────────────────
    PerpArbConfig strat_cfg;
    strat_cfg.trade_qty           = app.trade_qty;
    strat_cfg.mm_spread_target_bps = app.mm_spread;
    strat_cfg.taker_threshold_bps  = app.taker_spread;

    // ── Connectors (shadow or live) ──────────────────────────────────────────

    ShadowConfig shadow_cfg;
    std::snprintf(shadow_cfg.log_path, sizeof(shadow_cfg.log_path),
                  "shadow_decisions_%lld.jsonl",
                  (long long)std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch()).count());

    ShadowEngine shadow_engine(binance_book, kraken_book, shadow_cfg);

    // Strategy
    PerpArbStrategy strategy(
        strat_cfg,
        binance_book, kraken_book,
        shadow_engine.binance_connector(),
        shadow_engine.kraken_connector(),
        risk,
        order_mgr
    );

    // Wire fill callbacks: connector fills → order manager → strategy
    auto fill_cb = [&](const FillUpdate& u) {
        order_mgr.on_fill_update(u);
        strategy.on_fill(u);
    };
    shadow_engine.binance_connector().set_fill_callback(fill_cb);
    shadow_engine.kraken_connector().set_fill_callback(fill_cb);

    // Wire book updates → strategy + shadow fill check
    auto on_delta = [&]() {
        strategy.on_book_update();
        shadow_engine.check_fills();
    };

    binance_feed.set_delta_callback([&](const Delta& d) {
        binance_book.delta_handler()(d);
        on_delta();
    });
    binance_feed.set_snapshot_callback([&](const Snapshot& s) {
        binance_book.snapshot_handler()(s);
    });
    kraken_feed.set_delta_callback([&](const Delta& d) {
        kraken_book.delta_handler()(d);
        on_delta();
    });
    kraken_feed.set_snapshot_callback([&](const Snapshot& s) {
        kraken_book.snapshot_handler()(s);
    });

    // Error handlers: log and let the feed handler reconnect
    binance_feed.set_error_callback([](const std::string& e) {
        LOG_ERROR("Binance feed error", "msg", e.c_str());
    });
    kraken_feed.set_error_callback([](const std::string& e) {
        LOG_ERROR("Kraken futures feed error", "msg", e.c_str());
    });

    // ── Start feeds ───────────────────────────────────────────────────────────
    if (binance_feed.start() != Result::SUCCESS) {
        LOG_ERROR("Failed to start Binance feed");
        return 1;
    }
    if (kraken_feed.start() != Result::SUCCESS) {
        LOG_ERROR("Failed to start Kraken futures feed");
        return 1;
    }

    LOG_INFO("Both feeds started. Running...",
             "shadow", app.shadow_mode ? "yes" : "no");

    // ── Monitoring thread ─────────────────────────────────────────────────────
    const ShadowEngine* shadow_ptr = &shadow_engine;
    std::thread monitor_thread([&]() {
        monitor_loop(strategy, risk, shadow_ptr);
    });

    // ── Main event loop (heartbeat + shutdown check) ─────────────────────────
    // In production the feeds run their own IO threads via libwebsockets/Asio.
    // Here we spin, pumping heartbeats and checking the kill switch.
    using namespace std::chrono_literals;
    while (g_running.load(std::memory_order_acquire)) {
        risk.heartbeat();

        if (!risk.check_heartbeat()) {
            LOG_ERROR("Heartbeat missed — triggering kill switch");
            break;
        }
        if (risk.is_kill_switch_active()) {
            LOG_ERROR("Kill switch active — shutting down");
            break;
        }

        std::this_thread::sleep_for(1s);
    }

    // ── Graceful shutdown ─────────────────────────────────────────────────────
    g_running.store(false, std::memory_order_release);
    LOG_INFO("Shutting down...");

    binance_feed.stop();
    kraken_feed.stop();

    shadow_engine.print_summary();
    LOG_INFO("Final P&L",
             "realized_pnl",  strategy.realized_pnl(),
             "total_trades",  strategy.total_trades(),
             "shadow_net_pnl", shadow_engine.net_pnl());

    monitor_thread.join();
    return 0;
}
