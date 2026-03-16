#include "../execution/binance/binance_connector.hpp"
#include "../execution/coinbase/coinbase_connector.hpp"
#include "../execution/kraken/kraken_connector.hpp"
#include "../execution/okx/okx_connector.hpp"
#include "../execution/reconciliation_service.hpp"
#include "../execution/smart_order_router.hpp"
#include "../feeds/binance/binance_feed_handler.hpp"
#include "../feeds/coinbase/coinbase_feed_handler.hpp"
#include "../feeds/common/book_manager.hpp"
#include "../feeds/kraken/kraken_feed_handler.hpp"
#include "../feeds/okx/okx_feed_handler.hpp"
#include "../ipc/alpha_signal.hpp"
#include "../ipc/lob_publisher.hpp"
#include "../risk/circuit_breaker.hpp"
#include "../risk/global_risk_controls.hpp"
#include "../risk/kill_switch.hpp"
#include "../risk/risk_config_loader.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace {

struct CliOptions {
    std::string mode = "live";
    std::string venues = "BINANCE,KRAKEN,OKX,COINBASE";
    std::string symbol = "BTCUSDT";
    int loop_interval_ms = 100;
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
        else if (arg == "--loop-interval-ms" && i + 1 < argc)
            out.loop_interval_ms = std::stoi(argv[++i]);
    }
    return out;
}

std::atomic<bool> g_running{true};

bool has_venue(const std::string& csv, const std::string& needle) {
    std::stringstream ss(csv);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (item == needle)
            return true;
    }
    return false;
}

void stop_handler(int /*signum*/) { g_running.store(false, std::memory_order_release); }

void setup_signal_handlers() {
    std::signal(SIGINT, stop_handler);
    std::signal(SIGTERM, stop_handler);
}

trading::Order make_child_order(const char* symbol, trading::Exchange exchange, trading::Side side,
                                double qty, double price, uint64_t client_order_id) {
    trading::Order o;
    std::strncpy(o.symbol, symbol, sizeof(o.symbol) - 1);
    o.symbol[sizeof(o.symbol) - 1] = '\0';
    o.exchange = exchange;
    o.side = side;
    o.type = trading::OrderType::LIMIT;
    o.tif = trading::TimeInForce::IOC;
    o.quantity = qty;
    o.price = price;
    o.client_order_id = client_order_id;
    return o;
}

trading::VenueQuote make_quote(trading::Exchange exchange, const trading::BookManager& book,
                               bool enabled) {
    if (!enabled)
        return {exchange, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, false};

    const bool book_ready = book.is_ready();
    const double bid = book_ready ? book.best_bid() : 100.0;
    const double ask = book_ready ? book.best_ask() : 100.1;

    return {
        exchange, bid, ask, 0.40, 5.0, 0.5, 0.2, 0.70, 0.20, 0.4, true,
    };
}

} // namespace

int main(int argc, char** argv) {
    using namespace trading;

    const CliOptions opts = parse_args(argc, argv);

    RiskRuntimeConfig risk_cfg;
    const std::string risk_path = "config/" + opts.mode + "/risk.yaml";
    (void)RiskConfigLoader::load(risk_path, risk_cfg);

    KillSwitch kill_switch(risk_cfg.heartbeat_timeout_ns);
    CircuitBreaker circuit_breaker(risk_cfg.circuit_breaker, kill_switch);
    GlobalRiskControls global_risk(risk_cfg.global_risk, kill_switch);

    setup_signal_handlers();

    if (kill_switch.is_active())
        return 2;

    const bool run_binance = has_venue(opts.venues, "BINANCE");
    const bool run_kraken = has_venue(opts.venues, "KRAKEN");
    const bool run_okx = has_venue(opts.venues, "OKX");
    const bool run_coinbase = has_venue(opts.venues, "COINBASE");

    BookManager binance_book(opts.symbol, Exchange::BINANCE, 0.1, 10000);
    BookManager kraken_book(opts.symbol, Exchange::KRAKEN, 0.1, 10000);
    BookManager okx_book(opts.symbol, Exchange::OKX, 0.1, 10000);
    BookManager coinbase_book(opts.symbol, Exchange::COINBASE, 0.1, 10000);

    LobPublisher lob_publisher;
    if (!lob_publisher.open()) {
        std::cout << "[WARN] LOB publisher unavailable path=/tmp/trt_lob_feed.bin\n";
    }

    binance_book.set_publisher(lob_publisher.is_open() ? &lob_publisher : nullptr);
    kraken_book.set_publisher(lob_publisher.is_open() ? &lob_publisher : nullptr);
    okx_book.set_publisher(lob_publisher.is_open() ? &lob_publisher : nullptr);
    coinbase_book.set_publisher(lob_publisher.is_open() ? &lob_publisher : nullptr);

    BinanceFeedHandler binance_feed(opts.symbol);
    KrakenFeedHandler kraken_feed(opts.symbol);
    OkxFeedHandler okx_feed(opts.symbol);
    CoinbaseFeedHandler coinbase_feed(opts.symbol);

    binance_feed.set_snapshot_callback(binance_book.snapshot_handler());
    binance_feed.set_delta_callback(binance_book.delta_handler());
    kraken_feed.set_snapshot_callback(kraken_book.snapshot_handler());
    kraken_feed.set_delta_callback(kraken_book.delta_handler());
    okx_feed.set_snapshot_callback(okx_book.snapshot_handler());
    okx_feed.set_delta_callback(okx_book.delta_handler());
    coinbase_feed.set_snapshot_callback(coinbase_book.snapshot_handler());
    coinbase_feed.set_delta_callback(coinbase_book.delta_handler());

    if (run_binance)
        (void)binance_feed.start();
    if (run_kraken)
        (void)kraken_feed.start();
    if (run_okx)
        (void)okx_feed.start();
    if (run_coinbase)
        (void)coinbase_feed.start();

    BinanceConnector binance(http::env_var("BINANCE_API_KEY"), http::env_var("BINANCE_API_SECRET"),
                             opts.mode == "shadow" ? "mock://binance" : "https://api.binance.com");
    KrakenConnector kraken(http::env_var("KRAKEN_API_KEY"), http::env_var("KRAKEN_API_SECRET"),
                           opts.mode == "shadow" ? "mock://kraken" : "https://api.kraken.com");
    OkxConnector okx(http::env_var("OKX_API_KEY"), http::env_var("OKX_API_SECRET"),
                     opts.mode == "shadow" ? "mock://okx" : "https://www.okx.com");
    CoinbaseConnector coinbase(
        http::env_var("COINBASE_API_KEY"), http::env_var("COINBASE_API_SECRET"),
        opts.mode == "shadow" ? "mock://coinbase" : "https://api.coinbase.com");

    ReconciliationService reconciliation;
    if (run_binance)
        (void)reconciliation.register_connector(binance);
    if (run_kraken)
        (void)reconciliation.register_connector(kraken);
    if (run_okx)
        (void)reconciliation.register_connector(okx);
    if (run_coinbase)
        (void)reconciliation.register_connector(coinbase);

    auto connect_if_needed = [](LiveConnectorBase& connector, bool enabled, bool& reconnected) {
        if (!enabled)
            return ConnectorResult::OK;
        if (connector.is_connected())
            return ConnectorResult::OK;

        const ConnectorResult res = connector.connect();
        if (res == ConnectorResult::OK)
            reconnected = true;
        return res;
    };

    bool any_reconnected = false;
    (void)connect_if_needed(binance, run_binance, any_reconnected);
    (void)connect_if_needed(kraken, run_kraken, any_reconnected);
    (void)connect_if_needed(okx, run_okx, any_reconnected);
    (void)connect_if_needed(coinbase, run_coinbase, any_reconnected);

    if (any_reconnected) {
        const ConnectorResult reconcile_res = reconciliation.reconcile_on_reconnect();
        if (reconcile_res != ConnectorResult::OK) {
            std::cout << "reconcile_on_reconnect failed code=" << static_cast<int>(reconcile_res)
                      << "\n";
        }
    }

    AlphaSignalReader alpha_reader;
    alpha_reader.open();
    SmartOrderRouter sor;

    const auto reconnect_interval = std::chrono::seconds(1);
    const auto reconciliation_interval = std::chrono::seconds(30);
    const auto loop_interval =
        std::chrono::milliseconds(opts.loop_interval_ms > 0 ? opts.loop_interval_ms : 100);
    auto next_reconnect = std::chrono::steady_clock::now() + reconnect_interval;
    auto next_reconciliation = std::chrono::steady_clock::now() + reconciliation_interval;

    uint64_t next_id = 1;

    std::cout << "trading_engine started mode=" << opts.mode << " venues=" << opts.venues
              << " symbol=" << opts.symbol << "\n";

    while (g_running.load(std::memory_order_acquire)) {
        if (kill_switch.is_active())
            break;

        kill_switch.heartbeat();
        (void)kill_switch.check_heartbeat();

        const AlphaSignal alpha_signal = alpha_reader.read();
        std::array<VenueQuote, SmartOrderRouter::MAX_VENUES> venue_quotes{{
            make_quote(Exchange::BINANCE, binance_book, run_binance),
            make_quote(Exchange::KRAKEN, kraken_book, run_kraken),
            make_quote(Exchange::OKX, okx_book, run_okx),
            make_quote(Exchange::COINBASE, coinbase_book, run_coinbase),
        }};

        const RoutingDecision decision =
            sor.route_with_alpha(Side::BID, 0.5, alpha_signal, venue_quotes);
        if (!decision.blocked_by_alpha) {
            for (size_t i = 0; i < decision.child_count; ++i) {
                const auto& child = decision.children[i];
                const Order child_order =
                    make_child_order(opts.symbol.c_str(), child.exchange, Side::BID, child.quantity,
                                     child.limit_price, next_id++);

                const double signed_notional = child.quantity * child.limit_price;
                if (global_risk.commit_order(child.exchange, child_order.symbol, signed_notional) !=
                    GlobalRiskCheckResult::OK) {
                    std::cout << "global-risk blocked submit venue="
                              << exchange_to_string(child.exchange) << "\n";
                    continue;
                }

                if (circuit_breaker.check_drawdown(0.0) != CircuitCheckResult::OK) {
                    std::cout << "circuit-breaker blocked submit\n";
                    break;
                }

                ConnectorResult res = ConnectorResult::ERROR_UNKNOWN;
                switch (child.exchange) {
                case Exchange::BINANCE:
                    res = binance.submit_order(child_order);
                    break;
                case Exchange::KRAKEN:
                    res = kraken.submit_order(child_order);
                    break;
                case Exchange::OKX:
                    res = okx.submit_order(child_order);
                    break;
                case Exchange::COINBASE:
                    res = coinbase.submit_order(child_order);
                    break;
                default:
                    break;
                }

                std::cout << "child=" << i << " venue=" << exchange_to_string(child.exchange)
                          << " qty=" << child.quantity << " result=" << static_cast<int>(res)
                          << "\n";
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= next_reconnect) {
            bool recovered_connection = false;
            (void)connect_if_needed(binance, run_binance, recovered_connection);
            (void)connect_if_needed(kraken, run_kraken, recovered_connection);
            (void)connect_if_needed(okx, run_okx, recovered_connection);
            (void)connect_if_needed(coinbase, run_coinbase, recovered_connection);

            if (recovered_connection) {
                const ConnectorResult reconcile_res = reconciliation.reconcile_on_reconnect();
                if (reconcile_res != ConnectorResult::OK) {
                    std::cout << "reconcile_on_reconnect failed code="
                              << static_cast<int>(reconcile_res) << "\n";
                }
            }
            next_reconnect = now + reconnect_interval;
        }

        if (now >= next_reconciliation) {
            const ConnectorResult drift_res = reconciliation.run_periodic_drift_check();
            if (drift_res != ConnectorResult::OK) {
                std::cout << "periodic_reconciliation failed code=" << static_cast<int>(drift_res)
                          << "\n";
            }
            next_reconciliation = now + reconciliation_interval;
        }

        std::this_thread::sleep_for(loop_interval);
    }

    binance.disconnect();
    kraken.disconnect();
    okx.disconnect();
    coinbase.disconnect();
    binance_feed.stop();
    kraken_feed.stop();
    okx_feed.stop();
    coinbase_feed.stop();

    std::cout << "trading_engine shutdown complete\n";

    return 0;
}
