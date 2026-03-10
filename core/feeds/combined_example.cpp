#include "binance/binance_feed_handler.hpp"
#include "kraken/kraken_feed_handler.hpp"
#include "book_manager.hpp"
#include <iostream>
#include <iomanip>

using namespace trading;

static void print_book(const char* label, const BookManager& mgr) {
    std::cout << "\n=== " << label << " ===" << std::endl;
    if (!mgr.is_ready()) {
        std::cout << "  (not initialized)" << std::endl;
        return;
    }

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  Best bid : " << mgr.best_bid()  << std::endl;
    std::cout << "  Best ask : " << mgr.best_ask()  << std::endl;
    std::cout << "  Spread   : " << mgr.spread()    << std::endl;
    std::cout << "  Mid      : " << mgr.mid_price() << std::endl;

    std::vector<PriceLevel> bids, asks;
    mgr.get_top_levels(5, bids, asks);

    std::cout << "\n  Top bids:" << std::endl;
    for (const auto& l : bids) {
        std::cout << "    " << l.price << "  @  " << l.size << std::endl;
    }
    std::cout << "  Top asks:" << std::endl;
    for (const auto& l : asks) {
        std::cout << "    " << l.price << "  @  " << l.size << std::endl;
    }
}

int main() {
    Logger::min_level() = LogLevel::INFO;

    // BTC/USD with $1 tick, 20,000 levels → $20,000 range centered on best bid
    BookManager binance_book("BTCUSDT",  Exchange::BINANCE, 1.0, 20000);
    BookManager kraken_book ("XBTUSD",   Exchange::KRAKEN,  1.0, 20000);

    BinanceFeedHandler binance("BTCUSDT");
    KrakenFeedHandler  kraken("XBTUSD");

    binance.set_snapshot_callback(binance_book.snapshot_handler());
    binance.set_delta_callback   (binance_book.delta_handler());
    binance.set_error_callback   ([](const std::string& e) {
        std::cerr << "[Binance error] " << e << std::endl;
    });

    kraken.set_snapshot_callback(kraken_book.snapshot_handler());
    kraken.set_delta_callback   (kraken_book.delta_handler());
    kraken.set_error_callback   ([](const std::string& e) {
        std::cerr << "[Kraken error] " << e << std::endl;
    });

    std::cout << "Starting Binance feed..." << std::endl;
    if (binance.start() != Result::SUCCESS) {
        std::cerr << "Failed to start Binance handler" << std::endl;
        return 1;
    }

    std::cout << "Starting Kraken feed..." << std::endl;
    if (kraken.start() != Result::SUCCESS) {
        std::cerr << "Failed to start Kraken handler" << std::endl;
        return 1;
    }

    print_book("Binance BTCUSDT", binance_book);
    print_book("Kraken  XBTUSD",  kraken_book);

    std::cout << "\n=== Spread comparison ===" << std::endl;
    std::cout << std::fixed << std::setprecision(2);
    if (binance_book.is_ready() && kraken_book.is_ready()) {
        double cross_spread = kraken_book.best_ask() - binance_book.best_bid();
        std::cout << "  Binance mid : " << binance_book.mid_price() << std::endl;
        std::cout << "  Kraken  mid : " << kraken_book.mid_price()  << std::endl;
        std::cout << "  Cross spread (Kraken ask - Binance bid): " << cross_spread << std::endl;
    }

    binance.stop();
    kraken.stop();

    return 0;
}
