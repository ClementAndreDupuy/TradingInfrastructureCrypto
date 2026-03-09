#include "binance_feed_handler.hpp"
#include <iostream>
#include <thread>
#include <chrono>

using namespace trading;

int main() {
    Logger::min_level() = LogLevel::DEBUG;

    BinanceFeedHandler handler("BTCUSDT");

    handler.set_snapshot_callback([](const Snapshot& snapshot) {
        std::cout << "\n=== SNAPSHOT ===" << std::endl;
        std::cout << "Symbol: " << snapshot.symbol << std::endl;
        std::cout << "Sequence: " << snapshot.sequence << std::endl;

        std::cout << "\nBids:" << std::endl;
        for (size_t i = 0; i < std::min(size_t(5), snapshot.bids.size()); ++i) {
            std::cout << "  " << snapshot.bids[i].price << " @ " << snapshot.bids[i].size << std::endl;
        }

        std::cout << "\nAsks:" << std::endl;
        for (size_t i = 0; i < std::min(size_t(5), snapshot.asks.size()); ++i) {
            std::cout << "  " << snapshot.asks[i].price << " @ " << snapshot.asks[i].size << std::endl;
        }
    });

    handler.set_delta_callback([](const Delta& delta) {
        std::cout << "Delta: " << side_to_string(delta.side) << " "
                  << delta.price << " @ " << delta.size << " (seq=" << delta.sequence << ")" << std::endl;
    });

    handler.set_error_callback([](const std::string& error) {
        std::cerr << "ERROR: " << error << std::endl;
    });

    if (handler.start() != Result::SUCCESS) {
        std::cerr << "Failed to start handler" << std::endl;
        return 1;
    }

    std::cout << "\nReal-time data from Binance API" << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(1));

    handler.stop();

    std::cout << "\n=== Complete ===" << std::endl;
    return 0;
}
