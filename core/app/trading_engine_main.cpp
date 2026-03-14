#include "../execution/binance_connector.hpp"
#include "../execution/coinbase_connector.hpp"
#include "../execution/kraken_connector.hpp"
#include "../execution/okx_connector.hpp"
#include "../execution/smart_order_router.hpp"
#include "../risk/kill_switch.hpp"

#include <array>
#include <cstring>
#include <iostream>

namespace {

trading::Order make_parent_order(const char* symbol, trading::Side side, double qty) {
    trading::Order o;
    std::strncpy(o.symbol, symbol, sizeof(o.symbol) - 1);
    o.symbol[sizeof(o.symbol) - 1] = '\0';
    o.side = side;
    o.type = trading::OrderType::LIMIT;
    o.tif = trading::TimeInForce::IOC;
    o.quantity = qty;
    return o;
}

}  // namespace

int main() {
    using namespace trading;

    KillSwitch kill_switch;
    if (kill_switch.is_active()) return 2;

    BinanceConnector binance("", "", "mock://binance");
    KrakenConnector kraken("", "", "mock://kraken");
    OkxConnector okx("", "", "mock://okx");
    CoinbaseConnector coinbase("", "", "mock://coinbase");

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

    SmartOrderRouter sor;
    RoutingDecision decision = sor.route(Side::BID, 0.5, venues);

    Order parent = make_parent_order("BTCUSDT", Side::BID, 0.5);
    for (size_t i = 0; i < decision.child_count; ++i) {
        Order child = parent;
        child.exchange = decision.children[i].exchange;
        child.price = decision.children[i].limit_price;
        child.quantity = decision.children[i].quantity;
        child.client_order_id = static_cast<uint64_t>(i + 1);

        ConnectorResult res = ConnectorResult::ERROR_UNKNOWN;
        switch (child.exchange) {
            case Exchange::BINANCE: res = binance.submit_order(child); break;
            case Exchange::KRAKEN: res = kraken.submit_order(child); break;
            case Exchange::OKX: res = okx.submit_order(child); break;
            case Exchange::COINBASE: res = coinbase.submit_order(child); break;
            default: break;
        }

        std::cout << "child=" << i
                  << " venue=" << exchange_to_string(child.exchange)
                  << " qty=" << child.quantity
                  << " result=" << static_cast<int>(res) << "\n";
    }

    return 0;
}
