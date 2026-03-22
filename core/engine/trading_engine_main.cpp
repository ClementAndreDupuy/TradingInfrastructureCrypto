#include "../common/logging.hpp"
#include "../execution/binance/binance_connector.hpp"
#include "../execution/coinbase/coinbase_connector.hpp"
#include "../execution/kraken/kraken_connector.hpp"
#include "../execution/okx/okx_connector.hpp"
#include "../execution/common/reconciliation_service.hpp"
#include "../execution/router/smart_order_router.hpp"
#include "feed_bootstrap.hpp"
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
#include "../shadow/shadow_engine.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cmath>
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

    auto parse_args(int argc, char **argv) -> CliOptions {
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

    auto log_loaded_credential(const char *venue, const char *field, const std::string &value) -> void {
        if (value.empty()) {
            LOG_WARN("venue credential missing", "venue", venue, "field", field);
            return;
        }
        LOG_INFO("venue credential loaded", "venue", venue, "field", field,
                 "length", static_cast<int>(value.size()));
    }

    auto has_venue(const std::string &csv, const std::string &needle) -> bool {
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

    auto make_child_order(const char *symbol, trading::Exchange exchange, trading::Side side,
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

    auto top_opposite_depth(const trading::BookManager &book, trading::Side side) -> double {
        std::vector<trading::PriceLevel> bids;
        std::vector<trading::PriceLevel> asks;
        book.get_top_levels(1, bids, asks);
        if (side == trading::Side::BID) {
            return asks.empty() ? 0.0 : asks.front().size;
        }
        return bids.empty() ? 0.0 : bids.front().size;
    }

    auto price_for_quantity(const trading::BookManager &book, trading::Side side, double qty) -> double {
        std::vector<trading::PriceLevel> bids;
        std::vector<trading::PriceLevel> asks;
        book.get_top_levels(8, bids, asks);
        const auto &levels = side == trading::Side::BID ? asks : bids;
        double cumulative = 0.0;
        for (const auto &level: levels) {
            cumulative += level.size;
            if (cumulative + 1e-12 >= qty) {
                return level.price;
            }
        }
        return levels.empty() ? 0.0 : levels.back().price;
    }

    auto book_for_exchange(trading::Exchange exchange, trading::BookManager &binance_book,
                           trading::BookManager &kraken_book, trading::BookManager &okx_book,
                           trading::BookManager &coinbase_book) -> trading::BookManager * {
        switch (exchange) {
            case trading::Exchange::BINANCE:
                return &binance_book;
            case trading::Exchange::KRAKEN:
                return &kraken_book;
            case trading::Exchange::OKX:
                return &okx_book;
            case trading::Exchange::COINBASE:
                return &coinbase_book;
            default:
                return nullptr;
        }
    }

    auto make_quote(trading::Exchange exchange, const trading::BookManager &book,
                    bool enabled, trading::Side side = trading::Side::BID) -> trading::VenueQuote {
        if (!enabled) {
            return {exchange, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, false};
        }
        if (!book.is_ready()) {
            return {exchange, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, false};
        }

        const double bid = book.best_bid();
        const double ask = book.best_ask();
        const double side_depth = top_opposite_depth(book, side);

        return {
            exchange, bid, ask, side_depth, 5.0, 0.5, 0.2, 0.70, 0.20, 0.4, side_depth > 0.0,
        };
    }

    auto urgency_for_signal(const trading::AlphaSignal &signal, double current_position,
                            double target_position) -> trading::ShadowUrgency {
        const double delta = std::abs(target_position - current_position);
        if (signal.horizon_ticks <= 2 || delta >= 0.35 || signal.risk_score >= 0.55)
            return trading::ShadowUrgency::AGGRESSIVE;
        if (signal.horizon_ticks >= 8 && delta <= 0.10 && signal.risk_score < 0.35)
            return trading::ShadowUrgency::PASSIVE;
        return trading::ShadowUrgency::BALANCED;
    }

    auto build_intent_metadata(const char *intent, const char *reason, const trading::AlphaSignal &signal,
                               double current_position, double target_position,
                               double expected_cost_bps, double max_shortfall_bps)
            -> trading::ShadowIntentMetadata {
        trading::ShadowIntentMetadata metadata;
        std::strncpy(metadata.intent, intent, sizeof(metadata.intent) - 1);
        std::strncpy(metadata.reason, reason, sizeof(metadata.reason) - 1);
        metadata.signal_bps = signal.signal_bps;
        metadata.risk_score = signal.risk_score;
        metadata.current_position = current_position;
        metadata.target_position = target_position;
        metadata.expected_cost_bps = expected_cost_bps;
        metadata.expected_edge_bps = signal.signal_bps - expected_cost_bps;
        metadata.max_shortfall_bps = max_shortfall_bps;
        metadata.decision_ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count();
        metadata.urgency = urgency_for_signal(signal, current_position, target_position);
        return metadata;
    }
} // namespace

auto main(int argc, char **argv) -> int {
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

        BinanceFeedHandler binance_feed(opts.symbol);
        KrakenFeedHandler kraken_feed(opts.symbol);
        OkxFeedHandler okx_feed(opts.symbol);
        CoinbaseFeedHandler coinbase_feed(opts.symbol);

        constexpr double k_target_range_usd = 50.0;
        auto levels_for_tick = [](double tick) -> size_t {
            return static_cast<size_t>(k_target_range_usd / tick);
        };

        const double binance_tick = engine::refresh_tick_size_for_book_init(
            binance_feed, run_binance, "BINANCE", opts.symbol);
        const double kraken_tick = engine::refresh_tick_size_for_book_init(
            kraken_feed, run_kraken, "KRAKEN", opts.symbol);
        const double okx_tick =
                engine::refresh_tick_size_for_book_init(okx_feed, run_okx, "OKX", opts.symbol);
        const double coinbase_tick = engine::refresh_tick_size_for_book_init(
            coinbase_feed, run_coinbase, "COINBASE", opts.symbol);

        const size_t binance_levels = levels_for_tick(binance_tick);
        const size_t kraken_levels = levels_for_tick(kraken_tick);
        const size_t okx_levels = levels_for_tick(okx_tick);
        const size_t coinbase_levels = levels_for_tick(coinbase_tick);

        LOG_INFO("Grid configuration",
                 "target_range_usd", k_target_range_usd,
                 "binance_tick", binance_tick, "binance_levels", binance_levels, "binance_range_usd",
                 binance_levels * binance_tick,
                 "kraken_tick", kraken_tick, "kraken_levels", kraken_levels, "kraken_range_usd",
                 kraken_levels * kraken_tick,
                 "okx_tick", okx_tick, "okx_levels", okx_levels, "okx_range_usd", okx_levels * okx_tick,
                 "coinbase_tick", coinbase_tick, "coinbase_levels", coinbase_levels, "coinbase_range_usd",
                 coinbase_levels * coinbase_tick);

        BookManager binance_book(opts.symbol, Exchange::BINANCE, binance_tick, binance_levels);
        BookManager kraken_book(opts.symbol, Exchange::KRAKEN, kraken_tick, kraken_levels);
        BookManager okx_book(opts.symbol, Exchange::OKX, okx_tick, okx_levels);
        BookManager coinbase_book(opts.symbol, Exchange::COINBASE, coinbase_tick, coinbase_levels);

        LobPublisher lob_publisher;
        if (!lob_publisher.open()) {
            LOG_WARN("LOB publisher unavailable", "path", LobPublisher::k_default_path);
        }

        LobPublisher *pub = lob_publisher.is_open() ? &lob_publisher : nullptr;
        engine::wire_book_bridge_and_callbacks(binance_feed, binance_book, pub);
        engine::wire_book_bridge_and_callbacks(kraken_feed, kraken_book, pub);
        engine::wire_book_bridge_and_callbacks(okx_feed, okx_book, pub);
        engine::wire_book_bridge_and_callbacks(coinbase_feed, coinbase_book, pub);

        engine::start_feed_after_wiring(binance_feed, run_binance, "BINANCE", opts.symbol);
        engine::start_feed_after_wiring(kraken_feed, run_kraken, "KRAKEN", opts.symbol);
        engine::start_feed_after_wiring(okx_feed, run_okx, "OKX", opts.symbol);
        engine::start_feed_after_wiring(coinbase_feed, run_coinbase, "COINBASE", opts.symbol);

        const std::string binance_api_key = http::env_var("BINANCE_API_KEY");
        const std::string binance_api_secret = http::env_var("BINANCE_API_SECRET");
        const std::string kraken_api_key = http::env_var("KRAKEN_API_KEY");
        const std::string kraken_api_secret = http::env_var("KRAKEN_API_SECRET");
        const std::string okx_api_key = http::env_var("OKX_API_KEY");
        const std::string okx_api_secret = http::env_var("OKX_API_SECRET");
        const std::string okx_api_passphrase = http::env_var("OKX_API_PASSPHRASE");
        const std::string coinbase_api_key = http::env_var("COINBASE_API_KEY");
        const std::string coinbase_api_secret = http::env_var("COINBASE_API_SECRET");

        if (run_okx) {
            log_loaded_credential("OKX", "api_key", okx_api_key);
            log_loaded_credential("OKX", "api_secret", okx_api_secret);
            log_loaded_credential("OKX", "api_passphrase", okx_api_passphrase);
        }

        BinanceConnector binance(
            binance_api_key, binance_api_secret,
            opts.mode == "shadow" ? "mock://binance" : "https://api.binance.com");
        KrakenConnector kraken(kraken_api_key, kraken_api_secret,
                               opts.mode == "shadow" ? "mock://kraken" : "https://api.kraken.com");
        OkxConnector okx(okx_api_key, okx_api_secret, okx_api_passphrase,
                         opts.mode == "shadow" ? "mock://okx" : "https://www.okx.com");
        CoinbaseConnector coinbase(
            coinbase_api_key, coinbase_api_secret,
            opts.mode == "shadow" ? "mock://coinbase" : "https://api.coinbase.com");

        ShadowEngine shadow_engine(binance_book, kraken_book, okx_book, coinbase_book);

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

        auto connect_if_needed = [](LiveConnectorBase &connector, bool enabled,
                                    ReconciliationService &reconciliation) -> bool {
            if (!enabled) {
                return false;
            }
            if (connector.is_connected()) {
                return false;
            }

            const ConnectorResult res = connector.connect();
            if (res == ConnectorResult::OK) {
                (void) reconciliation.mark_reconnect_required(connector.exchange_id());
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
        uint64_t shadow_loop_index = 0;
        int64_t shadow_exit_loop = -1;
        AlphaSignal last_alpha_signal{};

        LOG_INFO("trading_engine started", "mode", opts.mode.c_str(), "venues", opts.venues.c_str(),
                 "symbol", opts.symbol.c_str());

        while (g_running.load(std::memory_order_acquire)) {
            if (kill_switch.is_active()) {
                break;
            }

            kill_switch.heartbeat();
            (void) kill_switch.check_heartbeat();

            const AlphaSignal alpha_signal = alpha_reader.read();
            last_alpha_signal = alpha_signal;
            if (opts.mode == "shadow") {
                shadow_engine.check_fills();
            }
            const auto submit_decision = [&](const RoutingDecision &decision, Side side,
                                             double route_qty, const ShadowIntentMetadata &intent_metadata) {
                for (size_t i = 0; i < decision.child_count; ++i) {
                    const auto &child = decision.children[i];

                    if (circuit_breaker.check_order_rate() != CircuitCheckResult::OK) {
                        LOG_WARN("circuit-breaker: order rate limit reached, halting submissions");
                        break;
                    }

                    BookManager *child_book =
                            book_for_exchange(child.exchange, binance_book, kraken_book, okx_book,
                                              coinbase_book);
                    if (child_book == nullptr) {
                        continue;
                    }
                    const double limit_price = price_for_quantity(*child_book, side, child.quantity);
                    if (limit_price <= 0.0) {
                        LOG_WARN("shadow routing skipped due to missing LOB depth", "venue",
                                 exchange_to_string(child.exchange), "qty", child.quantity);
                        continue;
                    }

                    const Order child_order =
                            make_child_order(opts.symbol.c_str(), child.exchange, side, child.quantity,
                                             limit_price, next_id++);

                    const double signed_notional = child.quantity * child_order.price *
                                                   (side == Side::BID ? 1.0 : -1.0);
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
                    if (opts.mode == "shadow") {
                        switch (child.exchange) {
                            case Exchange::BINANCE:
                                shadow_engine.binance_connector().set_intent_metadata(intent_metadata);
                                res = shadow_engine.binance_connector().submit_order(child_order);
                                break;
                            case Exchange::KRAKEN:
                                shadow_engine.kraken_connector().set_intent_metadata(intent_metadata);
                                res = shadow_engine.kraken_connector().submit_order(child_order);
                                break;
                            case Exchange::OKX:
                                shadow_engine.okx_connector().set_intent_metadata(intent_metadata);
                                res = shadow_engine.okx_connector().submit_order(child_order);
                                break;
                            case Exchange::COINBASE:
                                shadow_engine.coinbase_connector().set_intent_metadata(intent_metadata);
                                res = shadow_engine.coinbase_connector().submit_order(child_order);
                                break;
                            default:
                                break;
                        }
                    } else {
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
                    }

                    LOG_INFO("order submitted", "intent", intent_metadata.intent, "reason",
                             intent_metadata.reason, "child", static_cast<int>(i),
                             "venue", exchange_to_string(child.exchange), "side",
                             side == Side::BID ? "BID" : "ASK", "qty", child.quantity,
                             "route_qty", route_qty, "limit_px", child_order.price, "result",
                             static_cast<int>(res), "target_position",
                             intent_metadata.target_position, "current_position",
                             intent_metadata.current_position, "urgency",
                             static_cast<int>(intent_metadata.urgency), "expected_cost_bps",
                             intent_metadata.expected_cost_bps, "expected_edge_bps",
                             intent_metadata.expected_edge_bps, "max_shortfall_bps",
                             intent_metadata.max_shortfall_bps);
                }
            };

            if (opts.mode == "shadow") {
                ++shadow_loop_index;
                const double current_position = shadow_engine.net_position();
                const bool buy_signal = alpha_signal.signal_bps >= 3.0 &&
                                        alpha_signal.risk_score < 0.65;
                const double target_qty = 0.5 * alpha_signal.size_fraction;

                if (current_position > 1e-9) {
                    const bool exit_due_horizon = shadow_exit_loop >= 0 &&
                                                  static_cast<int64_t>(shadow_loop_index) >= shadow_exit_loop;
                    const bool exit_due_signal = !buy_signal;
                    if (exit_due_horizon || exit_due_signal) {
                        const double expected_cost_bps =
                                std::max(0.0, 5.0 + alpha_signal.risk_score * 4.0);
                        const ShadowIntentMetadata exit_metadata =
                                build_intent_metadata(exit_due_horizon ? "exit" : "reduce",
                                                      exit_due_horizon ? "horizon_expiry"
                                                                       : "signal_decay",
                                                      alpha_signal, current_position, 0.0,
                                                      expected_cost_bps, expected_cost_bps + 1.5);
                        constexpr Side exit_side = Side::ASK;
                        std::array<VenueQuote, SmartOrderRouter::MAX_VENUES> exit_quotes{
                            {
                                make_quote(Exchange::BINANCE, binance_book, run_binance, exit_side),
                                make_quote(Exchange::KRAKEN, kraken_book, run_kraken, exit_side),
                                make_quote(Exchange::OKX, okx_book, run_okx, exit_side),
                                make_quote(Exchange::COINBASE, coinbase_book, run_coinbase, exit_side),
                            }
                        };
                        const RoutingDecision exit_decision =
                                sor.route(exit_side, current_position, exit_quotes);
                        submit_decision(exit_decision, exit_side, current_position, exit_metadata);
                        shadow_exit_loop = -1;
                    }
                } else if (target_qty > 1e-6 && buy_signal) {
                    const double expected_cost_bps = std::max(0.0, 3.0 + alpha_signal.risk_score * 4.0);
                    const ShadowIntentMetadata entry_metadata =
                            build_intent_metadata("enter_long", "alpha_positive", alpha_signal,
                                                  current_position, target_qty, expected_cost_bps,
                                                  expected_cost_bps + 1.0);
                    constexpr Side entry_side = Side::BID;
                    std::array<VenueQuote, SmartOrderRouter::MAX_VENUES> entry_quotes{
                        {
                            make_quote(Exchange::BINANCE, binance_book, run_binance, entry_side),
                            make_quote(Exchange::KRAKEN, kraken_book, run_kraken, entry_side),
                            make_quote(Exchange::OKX, okx_book, run_okx, entry_side),
                            make_quote(Exchange::COINBASE, coinbase_book, run_coinbase, entry_side),
                        }
                    };
                    const RoutingDecision entry_decision =
                            sor.route(entry_side, target_qty, entry_quotes);
                    submit_decision(entry_decision, entry_side, target_qty, entry_metadata);
                    shadow_exit_loop = static_cast<int64_t>(shadow_loop_index) +
                                       (alpha_signal.horizon_ticks > 0 ? alpha_signal.horizon_ticks : 1);
                }
            } else {
                std::array<VenueQuote, SmartOrderRouter::MAX_VENUES> venue_quotes{
                    {
                        make_quote(Exchange::BINANCE, binance_book, run_binance, Side::BID),
                        make_quote(Exchange::KRAKEN, kraken_book, run_kraken, Side::BID),
                        make_quote(Exchange::OKX, okx_book, run_okx, Side::BID),
                        make_quote(Exchange::COINBASE, coinbase_book, run_coinbase, Side::BID),
                    }
                };

                const RoutingDecision decision =
                        sor.route_with_alpha(Side::BID, 0.5, alpha_signal, venue_quotes);
                if (!decision.blocked_by_alpha) {
                    const ShadowIntentMetadata live_metadata =
                            build_intent_metadata("enter_long", "alpha_positive", alpha_signal,
                                                  0.0, 0.5, 3.0 + alpha_signal.risk_score * 4.0,
                                                  5.0 + alpha_signal.risk_score * 4.0);
                    submit_decision(decision, Side::BID, 0.5, live_metadata);
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

        if (opts.mode == "shadow") {
            shadow_engine.check_fills();
            const double final_position = shadow_engine.net_position();
            if (final_position > 1e-9) {
                constexpr Side exit_side = Side::ASK;
                std::array<VenueQuote, SmartOrderRouter::MAX_VENUES> exit_quotes{
                    {
                        make_quote(Exchange::BINANCE, binance_book, run_binance, exit_side),
                        make_quote(Exchange::KRAKEN, kraken_book, run_kraken, exit_side),
                        make_quote(Exchange::OKX, okx_book, run_okx, exit_side),
                        make_quote(Exchange::COINBASE, coinbase_book, run_coinbase, exit_side),
                    }
                };
                const RoutingDecision exit_decision =
                        sor.route(exit_side, final_position, exit_quotes);
                const auto final_submit = [&](const RoutingDecision &decision) {
                    const ShadowIntentMetadata final_metadata =
                            build_intent_metadata("flatten", "session_end", last_alpha_signal,
                                                  final_position, 0.0, 6.0, 7.5);
                    for (size_t i = 0; i < decision.child_count; ++i) {
                        const auto &child = decision.children[i];
                        BookManager *child_book =
                                book_for_exchange(child.exchange, binance_book, kraken_book, okx_book,
                                                  coinbase_book);
                        if (child_book == nullptr) {
                            continue;
                        }
                        const double limit_price = price_for_quantity(*child_book, exit_side, child.quantity);
                        if (limit_price <= 0.0) {
                            continue;
                        }
                        const Order order = make_child_order(opts.symbol.c_str(), child.exchange,
                                                             exit_side, child.quantity,
                                                             limit_price, next_id++);
                        switch (child.exchange) {
                            case Exchange::BINANCE:
                                shadow_engine.binance_connector().set_intent_metadata(final_metadata);
                                (void) shadow_engine.binance_connector().submit_order(order);
                                break;
                            case Exchange::KRAKEN:
                                shadow_engine.kraken_connector().set_intent_metadata(final_metadata);
                                (void) shadow_engine.kraken_connector().submit_order(order);
                                break;
                            case Exchange::OKX:
                                shadow_engine.okx_connector().set_intent_metadata(final_metadata);
                                (void) shadow_engine.okx_connector().submit_order(order);
                                break;
                            case Exchange::COINBASE:
                                shadow_engine.coinbase_connector().set_intent_metadata(final_metadata);
                                (void) shadow_engine.coinbase_connector().submit_order(order);
                                break;
                            default:
                                break;
                        }
                    }
                };
                final_submit(exit_decision);
                shadow_engine.check_fills();
            }
            shadow_engine.log_summary();
        }

        LOG_INFO("trading_engine shutdown complete");

        return 0;
    } catch (const std::exception &ex) {
        LOG_ERROR("trading_engine fatal error", "what", ex.what());
    } catch (...) {
        LOG_ERROR("trading_engine fatal error", "what", "unknown exception");
    }

    return 1;
}
