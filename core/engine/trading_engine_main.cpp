#include "../common/logging.hpp"
#include "../execution/binance/binance_connector.hpp"
#include "../execution/coinbase/coinbase_connector.hpp"
#include "../execution/common/child_order_scheduler.hpp"
#include "../execution/common/parent_order_manager.hpp"
#include "../execution/kraken/kraken_connector.hpp"
#include "../execution/okx/okx_connector.hpp"
#include "../execution/common/portfolio_intent_engine.hpp"
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

    struct PortfolioIntentLogState {
        std::string intent;
        std::string reason;
        std::string reason_codes;
        double target_position = 0.0;
        bool flatten_now = false;
        std::chrono::steady_clock::time_point last_log_time{};
    };

    auto portfolio_intent_changed(const PortfolioIntentLogState &state,
                                  const trading::ShadowIntentMetadata &intent_metadata,
                                  const std::string &reason_codes,
                                  const trading::PortfolioIntent &intent) -> bool {
        return state.intent != intent_metadata.intent || state.reason != intent_metadata.reason ||
               state.reason_codes != reason_codes ||
               std::abs(state.target_position - intent.target_global_position) > 1e-6 ||
               state.flatten_now != intent.flatten_now;
    }

    void update_portfolio_intent_log_state(PortfolioIntentLogState &state,
                                           const trading::ShadowIntentMetadata &intent_metadata,
                                           const std::string &reason_codes,
                                           const trading::PortfolioIntent &intent,
                                           std::chrono::steady_clock::time_point now) {
        state.intent = intent_metadata.intent;
        state.reason = intent_metadata.reason;
        state.reason_codes = reason_codes;
        state.target_position = intent.target_global_position;
        state.flatten_now = intent.flatten_now;
        state.last_log_time = now;
    }

    auto make_child_order(const char *symbol, trading::Exchange exchange, trading::Side side,
                          double qty, double price, trading::TimeInForce tif,
                          uint64_t client_order_id) -> trading::Order {
        trading::Order order;
        std::strncpy(order.symbol, symbol, sizeof(order.symbol) - 1);
        order.symbol[sizeof(order.symbol) - 1] = '\0';
        order.exchange = exchange;
        order.side = side;
        order.type = trading::OrderType::LIMIT;
        order.tif = tif;
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

    auto join_reason_codes(const trading::PortfolioIntent &intent) -> std::string {
        if (intent.reason_count == 0)
            return trading::PortfolioIntentEngine::reason_code_to_string(
                trading::PortfolioIntentReasonCode::NONE);
        std::string joined;
        for (size_t i = 0; i < intent.reason_count; ++i) {
            if (!joined.empty())
                joined += ",";
            joined += trading::PortfolioIntentEngine::reason_code_to_string(intent.reason_codes[i]);
        }
        return joined;
    }

    auto intent_action(const trading::PortfolioIntent &intent) -> const char * {
        if (intent.flatten_now)
            return "flatten";
        if (intent.position_delta > 1e-9)
            return "enter_long";
        if (intent.position_delta < -1e-9)
            return "reduce";
        return "hold";
    }

    auto build_intent_metadata(const trading::AlphaSignal &signal,
                               double current_position,
                               const trading::PortfolioIntent &intent) -> trading::ShadowIntentMetadata {
        trading::ShadowIntentMetadata metadata;
        const std::string reasons = join_reason_codes(intent);
        std::strncpy(metadata.intent, intent_action(intent), sizeof(metadata.intent) - 1);
        std::strncpy(metadata.reason, reasons.c_str(), sizeof(metadata.reason) - 1);
        metadata.signal_bps = signal.signal_bps;
        metadata.risk_score = signal.risk_score;
        metadata.current_position = current_position;
        metadata.target_position = intent.target_global_position;
        metadata.expected_cost_bps = intent.expected_cost_bps;
        metadata.expected_edge_bps = signal.signal_bps - intent.expected_cost_bps;
        metadata.max_shortfall_bps = intent.max_shortfall_bps;
        metadata.decision_ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                .count();
        metadata.urgency = intent.urgency;
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

        const double target_range_usd = risk_cfg.target_range_usd > 0.0 ? risk_cfg.target_range_usd : 50.0;
        auto levels_for_tick = [target_range_usd](double tick) -> size_t {
            if (tick <= 0.0) {
                return 1;
            }
            const size_t levels = static_cast<size_t>(target_range_usd / tick);
            return levels > 0 ? levels : 1;
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
                 "target_range_usd", target_range_usd,
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
        RegimeSignalReader regime_reader;
        regime_reader.open();
        ParentOrderManager parent_manager;
        ChildOrderScheduler child_scheduler;
        PortfolioIntentEngine intent_engine;

        const auto reconnect_interval = std::chrono::seconds(1);
        const auto reconciliation_interval = std::chrono::seconds(30);
        const auto loop_interval =
                std::chrono::milliseconds(opts.loop_interval_ms > 0 ? opts.loop_interval_ms : 500);
        auto next_reconnect = std::chrono::steady_clock::now() + reconnect_interval;
        auto next_reconciliation = std::chrono::steady_clock::now() + reconciliation_interval;

        uint64_t next_id = 1;
        AlphaSignal last_alpha_signal{};
        PortfolioIntentLogState portfolio_log_state;
        constexpr auto portfolio_log_heartbeat = std::chrono::seconds(15);

        LOG_INFO("trading_engine started", "mode", opts.mode.c_str(), "venues", opts.venues.c_str(),
                 "symbol", opts.symbol.c_str());

        while (g_running.load(std::memory_order_acquire)) {
            if (kill_switch.is_active()) {
                break;
            }

            kill_switch.heartbeat();
            (void) kill_switch.check_heartbeat();

            const AlphaSignal alpha_signal = alpha_reader.read();
            const RegimeSignal regime_signal = regime_reader.read();
            last_alpha_signal = alpha_signal;
            if (opts.mode == "shadow") {
                shadow_engine.check_fills();
            }
            const auto submit_decision = [&](const SchedulerDecision &decision, Side side,
                                             double route_qty, const ShadowIntentMetadata &intent_metadata,
                                             const std::string &reason_codes) {
                const auto style_to_string = [](ChildExecutionStyle style) -> const char * {
                    switch (style) {
                        case ChildExecutionStyle::PASSIVE_JOIN:
                            return "passive_join";
                        case ChildExecutionStyle::PASSIVE_IMPROVE:
                            return "passive_improve";
                        case ChildExecutionStyle::IOC:
                            return "ioc";
                        case ChildExecutionStyle::SWEEP:
                            return "sweep";
                    }
                    return "ioc";
                };
                for (size_t i = 0; i < decision.routing.child_count; ++i) {
                    const auto &child = decision.routing.children[i];

                    if (circuit_breaker.check_order_rate() != CircuitCheckResult::OK) {
                        LOG_WARN("circuit-breaker: order rate limit reached, halting submissions");
                        break;
                    }

                    if (child.limit_price <= 0.0) {
                        LOG_WARN("shadow routing skipped due to missing LOB depth", "venue",
                                 exchange_to_string(child.exchange), "qty", child.quantity);
                        continue;
                    }

                    const Order child_order =
                            make_child_order(opts.symbol.c_str(), child.exchange, side, child.quantity,
                                             child.limit_price, child.tif, next_id++);

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
                             intent_metadata.reason, "reason_codes", reason_codes.c_str(), "child", static_cast<int>(i),
                             "style", style_to_string(decision.style), "venue",
                             exchange_to_string(child.exchange), "side",
                             side == Side::BID ? "BID" : "ASK", "qty", child.quantity,
                             "route_qty", route_qty, "limit_px", child_order.price, "result",
                             static_cast<int>(res), "target_position",
                             intent_metadata.target_position, "current_position",
                             intent_metadata.current_position, "urgency",
                             static_cast<int>(intent_metadata.urgency), "expected_cost_bps",
                             intent_metadata.expected_cost_bps, "expected_edge_bps",
                             intent_metadata.expected_edge_bps, "max_shortfall_bps",
                             intent_metadata.max_shortfall_bps, "expected_shortfall_bps",
                             decision.expected_shortfall_bps);
                }
            };

            const auto bid_quotes = std::array<VenueQuote, SmartOrderRouter::MAX_VENUES>{
                {
                    make_quote(Exchange::BINANCE, binance_book, run_binance, Side::BID),
                    make_quote(Exchange::KRAKEN, kraken_book, run_kraken, Side::BID),
                    make_quote(Exchange::OKX, okx_book, run_okx, Side::BID),
                    make_quote(Exchange::COINBASE, coinbase_book, run_coinbase, Side::BID),
                }
            };
            const auto ask_quotes = std::array<VenueQuote, SmartOrderRouter::MAX_VENUES>{
                {
                    make_quote(Exchange::BINANCE, binance_book, run_binance, Side::ASK),
                    make_quote(Exchange::KRAKEN, kraken_book, run_kraken, Side::ASK),
                    make_quote(Exchange::OKX, okx_book, run_okx, Side::ASK),
                    make_quote(Exchange::COINBASE, coinbase_book, run_coinbase, Side::ASK),
                }
            };

            PositionLedgerSnapshot portfolio_snapshot;
            portfolio_snapshot.global_position = opts.mode == "shadow" ? shadow_engine.net_position() : 0.0;
            if (portfolio_snapshot.global_position > 1e-9)
                portfolio_snapshot.oldest_inventory_age_ms = std::max<int64_t>(
                                                                 0, alpha_signal.horizon_ticks) * opts.loop_interval_ms;
            const PortfolioIntent intent =
                    intent_engine.evaluate(alpha_signal, regime_signal, portfolio_snapshot, bid_quotes);
            const ShadowIntentMetadata intent_metadata =
                    build_intent_metadata(alpha_signal, portfolio_snapshot.global_position, intent);
            const std::string reason_codes = join_reason_codes(intent);

            const auto now = std::chrono::steady_clock::now();
            const bool should_log_portfolio_intent =
                    portfolio_log_state.last_log_time == std::chrono::steady_clock::time_point{} ||
                    portfolio_intent_changed(portfolio_log_state, intent_metadata, reason_codes, intent) ||
                    now - portfolio_log_state.last_log_time >= portfolio_log_heartbeat;

            if (should_log_portfolio_intent) {
                update_portfolio_intent_log_state(portfolio_log_state, intent_metadata, reason_codes,
                                                  intent, now);
                LOG_INFO("portfolio intent", "intent", intent_metadata.intent, "reason",
                         intent_metadata.reason, "reason_codes", reason_codes.c_str(),
                         "signal_bps", alpha_signal.signal_bps, "risk_score", alpha_signal.risk_score,
                         "current_position", portfolio_snapshot.global_position, "target_position",
                         intent.target_global_position, "position_delta", intent.position_delta,
                         "expected_cost_bps", intent.expected_cost_bps, "max_shortfall_bps",
                         intent.max_shortfall_bps, "flatten_now", intent.flatten_now ? 1 : 0,
                         "urgency", static_cast<int>(intent.urgency));
            }

            if (opts.mode == "shadow") {
                const bool halted = kill_switch.is_active() ||
                                    circuit_breaker.check_consecutive_losses() !=
                                            CircuitCheckResult::OK;
                const ShadowStateTransition transition = shadow_engine.update_state(
                        intent.target_global_position, intent.flatten_now, halted,
                        reason_codes.c_str());
                if (transition.changed) {
                    LOG_INFO("shadow state transition", "from",
                             ShadowStateMachine::to_string(transition.previous), "to",
                             ShadowStateMachine::to_string(transition.current), "reason",
                             transition.reason, "current_pos", transition.current_position,
                             "target_pos", transition.target_position);
                }
            }

            const auto plan_update =
                    parent_manager.update_target(intent.position_delta, intent.urgency, now);
            const ParentExecutionPlan active_plan = plan_update.plan;
            if (plan_update.action != ParentPlanAction::NONE && active_plan.active() &&
                active_plan.remaining_qty > 1e-6) {
                const auto &scheduler_quotes =
                        active_plan.side == Side::BID ? bid_quotes : ask_quotes;
                const SchedulerDecision scheduled =
                        child_scheduler.schedule(active_plan, alpha_signal.horizon_ticks,
                                                 portfolio_snapshot.oldest_inventory_age_ms,
                                                 scheduler_quotes);
                submit_decision(scheduled, active_plan.side, active_plan.remaining_qty,
                                intent_metadata, reason_codes);
            }

            const auto reconciliation_now = std::chrono::steady_clock::now();
            if (run_reconciliation && reconciliation_now >= next_reconnect) {
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
                next_reconnect = reconciliation_now + reconnect_interval;
            }

            if (run_reconciliation && reconciliation_now >= next_reconciliation) {
                const ConnectorResult drift_res = reconciliation.run_periodic_drift_check();
                if (drift_res != ConnectorResult::OK) {
                    LOG_WARN("periodic_reconciliation failed", "code", static_cast<int>(drift_res));
                }
                next_reconciliation = reconciliation_now + reconciliation_interval;
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
                ParentExecutionPlan exit_plan;
                exit_plan.side = exit_side;
                exit_plan.total_qty = final_position;
                exit_plan.remaining_qty = final_position;
                exit_plan.urgency = ShadowUrgency::AGGRESSIVE;
                exit_plan.allow_aggressive = true;
                exit_plan.state = ParentPlanState::WORKING;
                const SchedulerDecision exit_decision =
                        child_scheduler.schedule(exit_plan, 1, 0, exit_quotes);
                const auto final_submit = [&](const SchedulerDecision &decision) {
                    PortfolioIntent final_intent;
                    final_intent.target_global_position = 0.0;
                    final_intent.position_delta = -final_position;
                    final_intent.flatten_now = true;
                    final_intent.expected_cost_bps = 6.0;
                    final_intent.max_shortfall_bps = 7.5;
                    final_intent.urgency = ShadowUrgency::AGGRESSIVE;
                    const ShadowIntentMetadata final_metadata =
                            build_intent_metadata(last_alpha_signal, final_position, final_intent);
                    for (size_t i = 0; i < decision.routing.child_count; ++i) {
                        const auto &child = decision.routing.children[i];
                        const double limit_price = child.limit_price;
                        if (limit_price <= 0.0) {
                            continue;
                        }
                        const Order order = make_child_order(opts.symbol.c_str(), child.exchange,
                                                             exit_side, child.quantity,
                                                             limit_price, child.tif, next_id++);
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
