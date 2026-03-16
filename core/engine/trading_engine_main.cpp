#include "../execution/binance/binance_connector.hpp"
#include "../execution/coinbase/coinbase_connector.hpp"
#include "../execution/kraken/kraken_connector.hpp"
#include "../execution/market_maker.hpp"
#include "../execution/okx/okx_connector.hpp"
#include "../execution/order_manager.hpp"
#include "../execution/reconciliation_service.hpp"
#include "../feeds/binance/binance_feed_handler.hpp"
#include "../feeds/coinbase/coinbase_feed_handler.hpp"
#include "../feeds/common/book_manager.hpp"
#include "../feeds/kraken/kraken_feed_handler.hpp"
#include "../feeds/okx/okx_feed_handler.hpp"
#include "../risk/circuit_breaker.hpp"
#include "../risk/global_risk_controls.hpp"
#include "../risk/kill_switch.hpp"
#include "../risk/risk_config_loader.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_shutdown{false};

void signal_handler(int) noexcept { g_shutdown.store(true, std::memory_order_release); }

struct CliOptions {
    std::string mode = "live";
    std::string venues = "BINANCE,KRAKEN,OKX,COINBASE";
    std::string symbol = "BTCUSDT";
};

CliOptions parse_args(int argc, char** argv) {
    CliOptions out;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--mode" && i + 1 < argc)
            out.mode = argv[++i];
        else if (arg == "--venues" && i + 1 < argc)
            out.venues = argv[++i];
        else if (arg == "--symbol" && i + 1 < argc)
            out.symbol = argv[++i];
    }
    return out;
}

trading::MarketMakerConfig make_mm_cfg(const std::string& symbol, trading::Exchange exchange) {
    trading::MarketMakerConfig cfg;
    std::strncpy(cfg.symbol, symbol.c_str(), sizeof(cfg.symbol) - 1);
    cfg.symbol[sizeof(cfg.symbol) - 1] = '\0';
    cfg.exchange = exchange;
    return cfg;
}

} // namespace

int main(int argc, char** argv) {
    using namespace trading;
    using clock = std::chrono::steady_clock;

    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT, signal_handler);

    const CliOptions opts = parse_args(argc, argv);
    const bool is_shadow = (opts.mode == "shadow");

    RiskRuntimeConfig risk_cfg;
    const std::string risk_path = "config/" + opts.mode + "/risk.yaml";
    (void)RiskConfigLoader::load(risk_path, risk_cfg);

    KillSwitch kill_switch(risk_cfg.heartbeat_timeout_ns);
    CircuitBreaker circuit_breaker(risk_cfg.circuit_breaker, kill_switch);
    GlobalRiskControls global_risk(risk_cfg.global_risk, kill_switch);

    if (kill_switch.is_active())
        return 2;

    // ── Book managers ────────────────────────────────────────────────────────
    BookManager binance_book(opts.symbol, Exchange::BINANCE, 0.1, 10000);
    BookManager kraken_book(opts.symbol, Exchange::KRAKEN, 0.1, 10000);
    BookManager okx_book(opts.symbol, Exchange::OKX, 0.1, 10000);
    BookManager coinbase_book(opts.symbol, Exchange::COINBASE, 0.1, 10000);

    // ── Execution connectors ─────────────────────────────────────────────────
    BinanceConnector binance_conn(
        http::env_var("BINANCE_API_KEY"), http::env_var("BINANCE_API_SECRET"),
        is_shadow ? "mock://binance" : "https://api.binance.com");
    KrakenConnector kraken_conn(
        http::env_var("KRAKEN_API_KEY"), http::env_var("KRAKEN_API_SECRET"),
        is_shadow ? "mock://kraken" : "https://api.kraken.com");
    OkxConnector okx_conn(http::env_var("OKX_API_KEY"), http::env_var("OKX_API_SECRET"),
                          is_shadow ? "mock://okx" : "https://www.okx.com");
    CoinbaseConnector coinbase_conn(
        http::env_var("COINBASE_API_KEY"), http::env_var("COINBASE_API_SECRET"),
        is_shadow ? "mock://coinbase" : "https://api.coinbase.com");

    binance_conn.connect();
    kraken_conn.connect();
    okx_conn.connect();
    coinbase_conn.connect();

    // ── Order managers ───────────────────────────────────────────────────────
    OrderManager binance_om(binance_conn);
    OrderManager kraken_om(kraken_conn);
    OrderManager okx_om(okx_conn);
    OrderManager coinbase_om(coinbase_conn);

    // ── Market makers ────────────────────────────────────────────────────────
    NeuralAlphaMarketMaker binance_mm(binance_om, binance_book, kill_switch, &circuit_breaker,
                                      make_mm_cfg(opts.symbol, Exchange::BINANCE));
    NeuralAlphaMarketMaker kraken_mm(kraken_om, kraken_book, kill_switch, &circuit_breaker,
                                     make_mm_cfg(opts.symbol, Exchange::KRAKEN));
    NeuralAlphaMarketMaker okx_mm(okx_om, okx_book, kill_switch, &circuit_breaker,
                                   make_mm_cfg(opts.symbol, Exchange::OKX));
    NeuralAlphaMarketMaker coinbase_mm(coinbase_om, coinbase_book, kill_switch, &circuit_breaker,
                                       make_mm_cfg(opts.symbol, Exchange::COINBASE));

    // ── Reconciliation service ────────────────────────────────────────────────
    ReconciliationService recon_service;
    recon_service.register_connector(binance_conn);
    recon_service.register_connector(kraken_conn);
    recon_service.register_connector(okx_conn);
    recon_service.register_connector(coinbase_conn);
    recon_service.set_risk_halt_hook(
        [&](Exchange, MismatchClass, std::string_view) { kill_switch.trigger(KillReason::DRAWDOWN); });
    recon_service.set_cancel_all_hook([&](Exchange ex, MismatchClass, std::string_view) {
        const char* sym = opts.symbol.c_str();
        switch (ex) {
        case Exchange::BINANCE:
            binance_conn.cancel_all(sym);
            break;
        case Exchange::KRAKEN:
            kraken_conn.cancel_all(sym);
            break;
        case Exchange::OKX:
            okx_conn.cancel_all(sym);
            break;
        case Exchange::COINBASE:
            coinbase_conn.cancel_all(sym);
            break;
        default:
            break;
        }
    });

    recon_service.reconcile_on_reconnect();

    // ── Feed handlers ────────────────────────────────────────────────────────
    BinanceFeedHandler binance_feed(opts.symbol, "", "",
                                    is_shadow ? "mock://binance" : "https://api.binance.com",
                                    is_shadow ? "" : "wss://stream.binance.com:9443/ws");
    KrakenFeedHandler kraken_feed(opts.symbol, "", "",
                                   is_shadow ? "mock://kraken" : "https://api.kraken.com",
                                   is_shadow ? "" : "wss://ws.kraken.com/v2");
    OkxFeedHandler okx_feed(opts.symbol, is_shadow ? "mock://okx" : "https://www.okx.com",
                             is_shadow ? "" : "wss://ws.okx.com:8443/ws/v5/public");
    CoinbaseFeedHandler coinbase_feed(opts.symbol,
                                       is_shadow ? "" : "wss://advanced-trade-ws.coinbase.com");

    // ── Wire feed → book → market maker ──────────────────────────────────────
    auto make_delta_cb = [](std::function<void(const Delta&)> book_delta,
                            NeuralAlphaMarketMaker& mm) {
        return [book_delta = std::move(book_delta), &mm](const Delta& d) {
            book_delta(d);
            mm.on_book_update();
        };
    };
    auto make_snapshot_cb = [](std::function<void(const Snapshot&)> book_snap,
                               NeuralAlphaMarketMaker& mm) {
        return [book_snap = std::move(book_snap), &mm](const Snapshot& s) {
            book_snap(s);
            mm.on_book_update();
        };
    };
    auto make_error_cb = [&](Exchange ex) {
        return [&, ex](const std::string& err) {
            LOG_WARN("feed error — triggering reconciliation", "exchange",
                     exchange_to_string(ex), "error", err.c_str());
            recon_service.reconcile_on_reconnect();
        };
    };

    binance_feed.set_snapshot_callback(
        make_snapshot_cb(binance_book.snapshot_handler(), binance_mm));
    binance_feed.set_delta_callback(make_delta_cb(binance_book.delta_handler(), binance_mm));
    binance_feed.set_error_callback(make_error_cb(Exchange::BINANCE));

    kraken_feed.set_snapshot_callback(make_snapshot_cb(kraken_book.snapshot_handler(), kraken_mm));
    kraken_feed.set_delta_callback(make_delta_cb(kraken_book.delta_handler(), kraken_mm));
    kraken_feed.set_error_callback(make_error_cb(Exchange::KRAKEN));

    okx_feed.set_snapshot_callback(make_snapshot_cb(okx_book.snapshot_handler(), okx_mm));
    okx_feed.set_delta_callback(make_delta_cb(okx_book.delta_handler(), okx_mm));
    okx_feed.set_error_callback(make_error_cb(Exchange::OKX));

    coinbase_feed.set_snapshot_callback(
        make_snapshot_cb(coinbase_book.snapshot_handler(), coinbase_mm));
    coinbase_feed.set_delta_callback(make_delta_cb(coinbase_book.delta_handler(), coinbase_mm));
    coinbase_feed.set_error_callback(make_error_cb(Exchange::COINBASE));

    binance_feed.start();
    kraken_feed.start();
    okx_feed.start();
    coinbase_feed.start();

    // ── Main event loop ───────────────────────────────────────────────────────
    // Heartbeat at 2 Hz (500 ms) — satisfies ≥ 1 Hz requirement.
    // Reconciliation every 30 s.
    constexpr auto heartbeat_interval = std::chrono::milliseconds(500);
    constexpr auto recon_interval = std::chrono::seconds(30);

    auto last_recon = clock::now();

    while (!g_shutdown.load(std::memory_order_acquire) && !kill_switch.is_active()) {
        kill_switch.heartbeat();

        const auto now = clock::now();
        if (now - last_recon >= recon_interval) {
            recon_service.run_periodic_drift_check();
            last_recon = now;
        }

        std::this_thread::sleep_for(heartbeat_interval);
    }

    // ── Shutdown ──────────────────────────────────────────────────────────────
    binance_feed.stop();
    kraken_feed.stop();
    okx_feed.stop();
    coinbase_feed.stop();

    binance_conn.disconnect();
    kraken_conn.disconnect();
    okx_conn.disconnect();
    coinbase_conn.disconnect();

    return 0;
}
