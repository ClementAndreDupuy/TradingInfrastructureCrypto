#include "../execution/binance_connector.hpp"
#include "../execution/coinbase_connector.hpp"
#include "../execution/kraken_connector.hpp"
#include "../execution/okx_connector.hpp"
#include "../execution/smart_order_router.hpp"
#include "../feeds/book_manager.hpp"
#include "../ipc/alpha_signal.hpp"
#include "../risk/circuit_breaker.hpp"
#include "../risk/kill_switch.hpp"

#include <array>
#include <cstring>
#include <iostream>
#include <string>

namespace {

struct CliOptions {
    std::string mode = "live";
    std::string venues = "BINANCE,KRAKEN,OKX,COINBASE";
    std::string symbol = "BTCUSDT";
};

CliOptions parse_args(int argc, char** argv) {
    CliOptions out;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--mode" && i + 1 < argc) out.mode = argv[++i];
        else if (arg == "--venues" && i + 1 < argc) out.venues = argv[++i];
        else if (arg == "--symbol" && i + 1 < argc) out.symbol = argv[++i];
    }
    return out;
}

trading::Order make_child_order(const char* symbol,
                                trading::Exchange exchange,
                                trading::Side side,
                                double qty,
                                double price,
                                uint64_t client_order_id) {
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

}  // namespace

int main(int argc, char** argv) {
    using namespace trading;

    const CliOptions opts = parse_args(argc, argv);

    KillSwitch kill_switch;
    CircuitBreakerConfig cb_cfg;
    CircuitBreaker circuit_breaker(cb_cfg, kill_switch);

    if (kill_switch.is_active()) return 2;

    BookManager binance_book(opts.symbol, Exchange::BINANCE, 0.1, 10000);
    BookManager kraken_book(opts.symbol, Exchange::KRAKEN, 0.1, 10000);
    BookManager okx_book(opts.symbol, Exchange::OKX, 0.1, 10000);
    BookManager coinbase_book(opts.symbol, Exchange::COINBASE, 0.1, 10000);

    (void)binance_book;
    (void)kraken_book;
    (void)okx_book;
    (void)coinbase_book;

    BinanceConnector binance(http::env_var("BINANCE_API_KEY"),
                             http::env_var("BINANCE_API_SECRET"),
                             opts.mode == "shadow" ? "mock://binance" : "https://api.binance.com");
    KrakenConnector kraken(http::env_var("KRAKEN_API_KEY"),
                           http::env_var("KRAKEN_API_SECRET"),
                           opts.mode == "shadow" ? "mock://kraken" : "https://api.kraken.com");
    OkxConnector okx(http::env_var("OKX_API_KEY"),
                     http::env_var("OKX_API_SECRET"),
                     opts.mode == "shadow" ? "mock://okx" : "https://www.okx.com");
    CoinbaseConnector coinbase(http::env_var("COINBASE_API_KEY"),
                               http::env_var("COINBASE_API_SECRET"),
                               opts.mode == "shadow" ? "mock://coinbase" : "https://api.coinbase.com");

    binance.connect();
    kraken.connect();
    okx.connect();
    coinbase.connect();

    std::array<VenueQuote, SmartOrderRouter::MAX_VENUES> venues{ {
        {Exchange::BINANCE,  100.0, 100.1, 0.40, 5.0, 0.5, 0.2, true},
        {Exchange::KRAKEN,   100.0, 100.2, 0.40, 4.0, 0.7, 0.3, true},
        {Exchange::OKX,      100.0, 100.0, 0.30, 5.0, 0.4, 0.4, true},
        {Exchange::COINBASE, 100.0, 100.3, 3.00, 6.0, 0.9, 0.5, true},
    } };

    AlphaSignalReader alpha_reader;
    alpha_reader.open();
    const AlphaSignal alpha_signal = alpha_reader.read();

    SmartOrderRouter sor;
    const RoutingDecision decision = sor.route_with_alpha(Side::BID, 0.5, alpha_signal, venues);
    if (decision.blocked_by_alpha) {
        std::cout << "alpha-gate=blocked signal=" << alpha_signal.signal_bps
                  << " risk=" << alpha_signal.risk_score << "\n";
        return 0;
    }

    if (circuit_breaker.check_drawdown(0.0) != CircuitCheckResult::OK) {
        std::cout << "circuit-breaker blocked submit\n";
        return 3;
    }

    uint64_t next_id = 1;
    for (size_t i = 0; i < decision.child_count; ++i) {
        const auto& child = decision.children[i];
        const Order child_order = make_child_order(opts.symbol.c_str(),
                                                   child.exchange,
                                                   Side::BID,
                                                   child.quantity,
                                                   child.limit_price,
                                                   next_id++);

        ConnectorResult res = ConnectorResult::ERROR_UNKNOWN;
        switch (child.exchange) {
            case Exchange::BINANCE: res = binance.submit_order(child_order); break;
            case Exchange::KRAKEN: res = kraken.submit_order(child_order); break;
            case Exchange::OKX: res = okx.submit_order(child_order); break;
            case Exchange::COINBASE: res = coinbase.submit_order(child_order); break;
            default: break;
        }

        std::cout << "child=" << i
                  << " venue=" << exchange_to_string(child.exchange)
                  << " qty=" << child.quantity
                  << " result=" << static_cast<int>(res) << "\n";
    }

    std::cout << "mode=" << opts.mode
              << " venues=" << opts.venues
              << " symbol=" << opts.symbol
              << " child_count=" << decision.child_count << "\n";

    return 0;
}
