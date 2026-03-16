#include "../execution/binance/binance_connector.hpp"
#include "../execution/coinbase/coinbase_connector.hpp"
#include "../execution/kraken/kraken_connector.hpp"
#include "../execution/market_maker.hpp"
#include "../execution/okx/okx_connector.hpp"
#include "../execution/order_manager.hpp"
#include "../execution/reconciliation_service.hpp"
#include "../execution/smart_order_router.hpp"
#include "../feeds/binance/binance_feed_handler.hpp"
#include "../feeds/coinbase/coinbase_feed_handler.hpp"
#include "../feeds/common/book_manager.hpp"
#include "../feeds/kraken/kraken_feed_handler.hpp"
#include "../feeds/okx/okx_feed_handler.hpp"
#include "../ipc/alpha_signal.hpp"
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

// Build a VenueQuote from a live BookManager.
// Fee and latency are venue-specific constants; other fields use
// per-book state where available and safe defaults elsewhere.
trading::VenueQuote venue_quote_from_book(const trading::BookManager& book,
                                          trading::Exchange exchange, double taker_fee_bps,
                                          double latency_penalty_bps) noexcept {
    trading::VenueQuote q;
    q.exchange = exchange;
    q.best_bid = book.best_bid();
    q.best_ask = book.best_ask();
    q.healthy = book.is_ready() && book.age_ms() < 1000;
    q.taker_fee_bps = taker_fee_bps;
    q.latency_penalty_bps = latency_penalty_bps;
    q.fill_probability = 0.5;
    q.depth_qty = 5.0;
    q.risk_penalty_bps = 0.0;
    q.queue_ahead_qty = 0.0;
    q.toxicity_bps = 0.0;
    return q;
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

    // ── Book managers (all 4 venues for full price discovery) ────────────────
    BookManager binance_book(opts.symbol, Exchange::BINANCE, 0.1, 10000);
    BookManager kraken_book(opts.symbol, Exchange::KRAKEN, 0.1, 10000);
    BookManager okx_book(opts.symbol, Exchange::OKX, 0.1, 10000);
    BookManager coinbase_book(opts.symbol, Exchange::COINBASE, 0.1, 10000);

    // ── Execution connectors (all 4 for reconciliation and cancel_all) ───────
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

    // ── Single market maker on the primary venue (Binance) ───────────────────
    // One strategy, one position. SOR decides routing on every tick.
    OrderManager order_mgr(binance_conn);

    MarketMakerConfig mm_cfg;
    std::strncpy(mm_cfg.symbol, opts.symbol.c_str(), sizeof(mm_cfg.symbol) - 1);
    mm_cfg.symbol[sizeof(mm_cfg.symbol) - 1] = '\0';
    mm_cfg.exchange = Exchange::BINANCE;

    NeuralAlphaMarketMaker market_maker(order_mgr, binance_book, kill_switch, &circuit_breaker,
                                        mm_cfg);

    // ── Smart order router ───────────────────────────────────────────────────
    SmartOrderRouter sor;
    AlphaSignalReader alpha_reader;
    alpha_reader.open();

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

    // ── Shared book-update logic ──────────────────────────────────────────────
    // Called after any venue's book changes. Uses SOR to alpha-gate the market
    // maker: if the alpha signal is too weak or risk is elevated, skip quoting.
    // VenueQuotes are built from live books on every call so the SOR always
    // routes on current market state.
    auto on_any_book_update = [&]() {
        const std::array<VenueQuote, SmartOrderRouter::MAX_VENUES> venues{{
            venue_quote_from_book(binance_book, Exchange::BINANCE, 4.0, 0.5),
            venue_quote_from_book(kraken_book, Exchange::KRAKEN, 4.0, 0.7),
            venue_quote_from_book(okx_book, Exchange::OKX, 3.0, 0.4),
            venue_quote_from_book(coinbase_book, Exchange::COINBASE, 30.0, 0.9),
        }};

        const AlphaSignal alpha = alpha_reader.read();
        const RoutingDecision decision =
            sor.route_with_alpha(Side::BID, mm_cfg.order_qty, alpha, venues);

        if (decision.blocked_by_alpha)
            return;

        market_maker.on_book_update();
    };

    // ── Feed handlers ─────────────────────────────────────────────────────────
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

    auto make_error_cb = [&](Exchange ex) {
        return [&, ex](const std::string& err) {
            LOG_WARN("feed error — triggering reconciliation", "exchange",
                     exchange_to_string(ex), "error", err.c_str());
            recon_service.reconcile_on_reconnect();
        };
    };

    // Binance is the primary book: snapshot/delta update primary book and fire
    // the market maker through SOR gating.
    binance_feed.set_snapshot_callback([&](const Snapshot& s) {
        binance_book.snapshot_handler()(s);
        on_any_book_update();
    });
    binance_feed.set_delta_callback([&](const Delta& d) {
        binance_book.delta_handler()(d);
        on_any_book_update();
    });
    binance_feed.set_error_callback(make_error_cb(Exchange::BINANCE));

    // Secondary books update price data used by SOR but also trigger the
    // market maker (cross-venue price moves may warrant requoting on Binance).
    kraken_feed.set_snapshot_callback([&](const Snapshot& s) {
        kraken_book.snapshot_handler()(s);
        on_any_book_update();
    });
    kraken_feed.set_delta_callback([&](const Delta& d) {
        kraken_book.delta_handler()(d);
        on_any_book_update();
    });
    kraken_feed.set_error_callback(make_error_cb(Exchange::KRAKEN));

    okx_feed.set_snapshot_callback([&](const Snapshot& s) {
        okx_book.snapshot_handler()(s);
        on_any_book_update();
    });
    okx_feed.set_delta_callback([&](const Delta& d) {
        okx_book.delta_handler()(d);
        on_any_book_update();
    });
    okx_feed.set_error_callback(make_error_cb(Exchange::OKX));

    coinbase_feed.set_snapshot_callback([&](const Snapshot& s) {
        coinbase_book.snapshot_handler()(s);
        on_any_book_update();
    });
    coinbase_feed.set_delta_callback([&](const Delta& d) {
        coinbase_book.delta_handler()(d);
        on_any_book_update();
    });
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
