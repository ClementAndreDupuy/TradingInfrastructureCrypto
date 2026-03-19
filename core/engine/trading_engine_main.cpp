#include "../common/logging.hpp"
#include "../execution/binance/binance_connector.hpp"
#include "../execution/coinbase/coinbase_connector.hpp"
#include "../execution/kraken/kraken_connector.hpp"
#include "../execution/okx/okx_connector.hpp"
#include "../execution/common/reconciliation_service.hpp"
#include "../execution/router/smart_order_router.hpp"
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
#include <exception>
#include <sstream>
#include <string>
#include <thread>

namespace {

struct CliOptions {
    std::string mode = "live";
    std::string venues = "BINANCE,KRAKEN,OKX,COINBASE";
    std::string symbol = "BTCUSDT";
    int loop_interval_ms = 500;
};

auto parse_args(int argc, char** argv) -> CliOptions {
    CliOptions out;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--mode" && i + 1 < argc) {
            out.mode = argv[++i];
        } else if (arg == "--venues" && i + 1 < argc) {
            out.venues = argv[++i];
        } else if (arg == "--symbol" && i + 1 < argc) {
            out.symbol = argv[++i];
        } else if (arg == "--loop-interval-ms" && i + 1 < argc) {
            try {
                out.loop_interval_ms = std::stoi(argv[++i]);
            } catch (...) {
                LOG_WARN("parse_args: invalid --loop-interval-ms value, using default", "value",
                         argv[i], "default_ms", out.loop_interval_ms);
            }
        }
    }
    return out;
}

std::atomic<bool> g_running{true};


auto has_venue(const std::string& csv, const std::string& needle) -> bool {
    std::stringstream csv_stream(csv);
    std::string item;
    while (std::getline(csv_stream, item, ',')) {
        if (item == needle) {
            return true;
        }
    }
    return false;
}

void stop_handler(int /*signum*/) { g_running.store(false, std::memory_order_release); }

void setup_signal_handlers() {
    std::signal(SIGINT, stop_handler);
    std::signal(SIGTERM, stop_handler);
}

auto make_child_order(const char* symbol, trading::Exchange exchange, trading::Side side,
                      double qty, double price, uint64_t client_order_id) -> trading::Order {
    trading::Order order;
    std::strncpy(order.symbol, symbol, sizeof(order.symbol) - 1);
    order.symbol[sizeof(order.symbol) - 1] = '\0';
    order.exchange = exchange;
    order.side = side;
    order.type = trading::OrderType::LIMIT;
    order.tif = trading::TimeInForce::IOC;
    order.quantity = qty;
    order.price = price;
    order.client_order_id = client_order_id;
    return order;
}

auto make_quote(trading::Exchange exchange, const trading::BookManager& book,
                bool enabled) -> trading::VenueQuote {
    if (!enabled) {
        return {exchange, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, false};
    }
    if (!book.is_ready()) {
        return {exchange, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, false};
    }

    const double bid = book.best_bid();
    const double ask = book.best_ask();

    // TODO: source taker fees, latency/risk penalties, and fill-probability from
    //       per-venue config rather than using hardcoded placeholder values.
    return {
        exchange, bid, ask, 0.40, 5.0, 0.5, 0.2, 0.70, 0.20, 0.4, true,
    };
}

} // namespace

auto main(int argc, char** argv) -> int {
    try {
        using namespace trading;

        const CliOptions opts = parse_args(argc, argv);

        RiskRuntimeConfig risk_cfg;
        const std::string risk_path = "config/" + opts.mode + "/risk.yaml";
        if (!RiskConfigLoader::load(risk_path, risk_cfg)) {
            LOG_WARN("Risk config not loaded — using compiled-in defaults", "path",
                     risk_path.c_str());
        }

        KillSwitch kill_switch(risk_cfg.heartbeat_timeout_ns);
        CircuitBreaker circuit_breaker(risk_cfg.circuit_breaker, kill_switch);
        GlobalRiskControls global_risk(risk_cfg.global_risk, kill_switch);

        setup_signal_handlers();

        if (kill_switch.is_active()) {
            return 2;
        }

        const bool run_binance = has_venue(opts.venues, "BINANCE");
        const bool run_kraken = has_venue(opts.venues, "KRAKEN");
        const bool run_okx = has_venue(opts.venues, "OKX");
        const bool run_coinbase = has_venue(opts.venues, "COINBASE");

        // Construct feed handlers first so start() can fetch tick_size from the exchange
        // REST endpoint before we build the order-book grids.
        BinanceFeedHandler binance_feed(opts.symbol);
        KrakenFeedHandler kraken_feed(opts.symbol);
        OkxFeedHandler okx_feed(opts.symbol);
        CoinbaseFeedHandler coinbase_feed(opts.symbol);

        if (run_binance && binance_feed.start() != Result::SUCCESS) {
            LOG_WARN("Binance feed failed to start", "symbol", opts.symbol.c_str());
        }
        if (run_kraken && kraken_feed.start() != Result::SUCCESS) {
            LOG_WARN("Kraken feed failed to start", "symbol", opts.symbol.c_str());
        }
        if (run_okx && okx_feed.start() != Result::SUCCESS) {
            LOG_WARN("OKX feed failed to start", "symbol", opts.symbol.c_str());
        }
        if (run_coinbase && coinbase_feed.start() != Result::SUCCESS) {
            LOG_WARN("Coinbase feed failed to start", "symbol", opts.symbol.c_str());
        }

        // Derive grid precision from the tick_size each feed fetched from its exchange.
        // Level count is scaled per exchange so that every grid covers the same USD range
        // (~$2 000), regardless of tick size.  Exchanges with a finer tick (e.g. 0.01) get
        // more levels; coarser ticks (e.g. 0.1) get fewer.  This prevents Binance/Coinbase
        // grids from recentering 10× more often than Kraken/OKX grids.
        // Memory: worst case 200 000 levels × 8 B × 4 arrays = ~6.4 MB per BookManager.
        constexpr double k_target_range_usd  = 2'000.0;
        constexpr double k_fallback_tick_size = 1.0;
        auto effective_tick   = [](double ts) { return ts > 0.0 ? ts : k_fallback_tick_size; };
        auto levels_for_tick  = [](double tick) -> size_t {
            return static_cast<size_t>(k_target_range_usd / tick);
        };

        const double binance_tick  = effective_tick(binance_feed.tick_size());
        const double kraken_tick   = effective_tick(kraken_feed.tick_size());
        const double okx_tick      = effective_tick(okx_feed.tick_size());
        const double coinbase_tick = effective_tick(coinbase_feed.tick_size());

        const size_t binance_levels  = levels_for_tick(binance_tick);
        const size_t kraken_levels   = levels_for_tick(kraken_tick);
        const size_t okx_levels      = levels_for_tick(okx_tick);
        const size_t coinbase_levels = levels_for_tick(coinbase_tick);

        LOG_INFO("Grid configuration",
                 "target_range_usd", k_target_range_usd,
                 "binance_tick",  binance_tick,  "binance_levels",  binance_levels,  "binance_range_usd",  binance_levels  * binance_tick,
                 "kraken_tick",   kraken_tick,   "kraken_levels",   kraken_levels,   "kraken_range_usd",   kraken_levels   * kraken_tick,
                 "okx_tick",      okx_tick,      "okx_levels",      okx_levels,      "okx_range_usd",      okx_levels      * okx_tick,
                 "coinbase_tick", coinbase_tick, "coinbase_levels", coinbase_levels, "coinbase_range_usd", coinbase_levels * coinbase_tick);

        BookManager binance_book(opts.symbol, Exchange::BINANCE,  binance_tick,  binance_levels);
        BookManager kraken_book(opts.symbol,  Exchange::KRAKEN,   kraken_tick,   kraken_levels);
        BookManager okx_book(opts.symbol,     Exchange::OKX,      okx_tick,      okx_levels);
        BookManager coinbase_book(opts.symbol, Exchange::COINBASE, coinbase_tick, coinbase_levels);

        LobPublisher lob_publisher;
        if (!lob_publisher.open()) {
            LOG_WARN("LOB publisher unavailable", "path", LobPublisher::k_default_path);
        }

        LobPublisher* pub = lob_publisher.is_open() ? &lob_publisher : nullptr;
        binance_book.set_publisher(pub);
        kraken_book.set_publisher(pub);
        okx_book.set_publisher(pub);
        coinbase_book.set_publisher(pub);

        binance_feed.set_snapshot_callback(binance_book.snapshot_handler());
        binance_feed.set_delta_callback(binance_book.delta_handler());
        kraken_feed.set_snapshot_callback(kraken_book.snapshot_handler());
        kraken_feed.set_delta_callback(kraken_book.delta_handler());
        okx_feed.set_snapshot_callback(okx_book.snapshot_handler());
        okx_feed.set_delta_callback(okx_book.delta_handler());
        coinbase_feed.set_snapshot_callback(coinbase_book.snapshot_handler());
        coinbase_feed.set_delta_callback(coinbase_book.delta_handler());

        BinanceConnector binance(
            http::env_var("BINANCE_API_KEY"), http::env_var("BINANCE_API_SECRET"),
            opts.mode == "shadow" ? "mock://binance" : "https://api.binance.com");
        KrakenConnector kraken(http::env_var("KRAKEN_API_KEY"), http::env_var("KRAKEN_API_SECRET"),
                               opts.mode == "shadow" ? "mock://kraken" : "https://api.kraken.com");
        OkxConnector okx(http::env_var("OKX_API_KEY"), http::env_var("OKX_API_SECRET"),
                         opts.mode == "shadow" ? "mock://okx" : "https://www.okx.com");
        CoinbaseConnector coinbase(
            http::env_var("COINBASE_API_KEY"), http::env_var("COINBASE_API_SECRET"),
            opts.mode == "shadow" ? "mock://coinbase" : "https://api.coinbase.com");

        ReconciliationService reconciliation;
        if (run_binance && !reconciliation.register_connector(binance)) {
            LOG_WARN("Failed to register Binance connector with reconciliation service");
        }
        if (run_kraken && !reconciliation.register_connector(kraken)) {
            LOG_WARN("Failed to register Kraken connector with reconciliation service");
        }
        if (run_okx && !reconciliation.register_connector(okx)) {
            LOG_WARN("Failed to register OKX connector with reconciliation service");
        }
        if (run_coinbase && !reconciliation.register_connector(coinbase)) {
            LOG_WARN("Failed to register Coinbase connector with reconciliation service");
        }

        auto connect_if_needed = [](LiveConnectorBase& connector, bool enabled,
                                    ReconciliationService& reconciliation) -> bool {
            if (!enabled) {
                return false;
            }
            if (connector.is_connected()) {
                return false;
            }

            const ConnectorResult res = connector.connect();
            if (res == ConnectorResult::OK) {
                (void)reconciliation.mark_reconnect_required(connector.exchange_id());
            }
            return res == ConnectorResult::OK;
        };

        const bool run_reconciliation = (opts.mode != "shadow");

        bool any_reconnected = false;
        if (run_reconciliation) {
            any_reconnected |= connect_if_needed(binance, run_binance, reconciliation);
            any_reconnected |= connect_if_needed(kraken, run_kraken, reconciliation);
            any_reconnected |= connect_if_needed(okx, run_okx, reconciliation);
            any_reconnected |= connect_if_needed(coinbase, run_coinbase, reconciliation);
        }

        if (any_reconnected) {
            const ConnectorResult reconcile_res = reconciliation.reconcile_on_reconnect();
            if (reconcile_res != ConnectorResult::OK) {
                LOG_WARN("reconcile_on_reconnect failed", "code", static_cast<int>(reconcile_res));
            }
        }

        AlphaSignalReader alpha_reader;
        alpha_reader.open();
        SmartOrderRouter sor;

        const auto reconnect_interval = std::chrono::seconds(1);
        const auto reconciliation_interval = std::chrono::seconds(30);
        const auto loop_interval =
            std::chrono::milliseconds(opts.loop_interval_ms > 0 ? opts.loop_interval_ms : 500);
        auto next_reconnect = std::chrono::steady_clock::now() + reconnect_interval;
        auto next_reconciliation = std::chrono::steady_clock::now() + reconciliation_interval;

        uint64_t next_id = 1;

        LOG_INFO("trading_engine started", "mode", opts.mode.c_str(), "venues", opts.venues.c_str(),
                 "symbol", opts.symbol.c_str());

        while (g_running.load(std::memory_order_acquire)) {
            if (kill_switch.is_active()) {
                break;
            }

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

                    if (circuit_breaker.check_order_rate() != CircuitCheckResult::OK) {
                        LOG_WARN("circuit-breaker: order rate limit reached, halting submissions");
                        break;
                    }

                    const Order child_order =
                        make_child_order(opts.symbol.c_str(), child.exchange, Side::BID,
                                         child.quantity, child.limit_price, next_id++);

                    const double signed_notional = child.quantity * child.limit_price;
                    if (global_risk.commit_order(child.exchange, child_order.symbol,
                                                 signed_notional) != GlobalRiskCheckResult::OK) {
                        LOG_WARN("global-risk blocked submit", "venue",
                                 exchange_to_string(child.exchange));
                        continue;
                    }

                    if (circuit_breaker.check_drawdown(circuit_breaker.realized_pnl()) !=
                        CircuitCheckResult::OK) {
                        LOG_WARN("circuit-breaker: drawdown limit reached, halting submissions");
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

                    LOG_INFO("order submitted", "child", static_cast<int>(i), "venue",
                             exchange_to_string(child.exchange), "qty", child.quantity, "result",
                             static_cast<int>(res));
                }
            }

            const auto now = std::chrono::steady_clock::now();
            if (run_reconciliation && now >= next_reconnect) {
                bool recovered_connection = false;
                recovered_connection |= connect_if_needed(binance, run_binance, reconciliation);
                recovered_connection |= connect_if_needed(kraken, run_kraken, reconciliation);
                recovered_connection |= connect_if_needed(okx, run_okx, reconciliation);
                recovered_connection |= connect_if_needed(coinbase, run_coinbase, reconciliation);

                if (recovered_connection) {
                    const ConnectorResult reconcile_res = reconciliation.reconcile_on_reconnect();
                    if (reconcile_res != ConnectorResult::OK) {
                        LOG_WARN("reconcile_on_reconnect failed", "code",
                                 static_cast<int>(reconcile_res));
                    }
                }
                next_reconnect = now + reconnect_interval;
            }

            if (run_reconciliation && now >= next_reconciliation) {
                const ConnectorResult drift_res = reconciliation.run_periodic_drift_check();
                if (drift_res != ConnectorResult::OK) {
                    LOG_WARN("periodic_reconciliation failed", "code", static_cast<int>(drift_res));
                }
                next_reconciliation = now + reconciliation_interval;
            }

            std::this_thread::sleep_for(loop_interval);
        }

        if (run_binance) {
            binance.disconnect();
            binance_feed.stop();
        }
        if (run_kraken) {
            kraken.disconnect();
            kraken_feed.stop();
        }
        if (run_okx) {
            okx.disconnect();
            okx_feed.stop();
        }
        if (run_coinbase) {
            coinbase.disconnect();
            coinbase_feed.stop();
        }

        LOG_INFO("trading_engine shutdown complete");

        return 0;
    } catch (const std::exception& ex) {
        LOG_ERROR("trading_engine fatal error", "what", ex.what());
    } catch (...) {
        LOG_ERROR("trading_engine fatal error", "what", "unknown exception");
    }

    return 1;
}
