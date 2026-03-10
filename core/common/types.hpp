#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace trading {

// Exchange enumeration
enum class Exchange : uint8_t {
    BINANCE = 0,
    OKX = 1,
    COINBASE = 2,
    KRAKEN = 3,
    UNKNOWN = 255
};

// Order side
enum class Side : uint8_t {
    BID = 0,
    ASK = 1
};

// Result codes for error handling (no exceptions in hot path)
enum class Result : uint8_t {
    SUCCESS = 0,
    ERROR_INVALID_SEQUENCE = 1,
    ERROR_INVALID_PRICE = 2,
    ERROR_INVALID_SIZE = 3,
    ERROR_SEQUENCE_GAP = 4,
    ERROR_BOOK_CORRUPTED = 5,
    ERROR_CONNECTION_LOST = 6
};

// Price level in order book
struct PriceLevel {
    double price;
    double size;

    PriceLevel() : price(0.0), size(0.0) {}
    PriceLevel(double p, double s) : price(p), size(s) {}
};

// Book delta update
struct Delta {
    Side side;
    double price;
    double size;
    uint64_t sequence;
    int64_t timestamp_exchange_ns;  // Exchange timestamp in nanoseconds
    int64_t timestamp_local_ns;     // Local receipt timestamp (PTP)

    Delta()
        : side(Side::BID),
          price(0.0),
          size(0.0),
          sequence(0),
          timestamp_exchange_ns(0),
          timestamp_local_ns(0) {}
};

// Order book snapshot
struct Snapshot {
    std::string symbol;
    Exchange exchange;
    uint64_t sequence;
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
    int64_t timestamp_exchange_ns;
    int64_t timestamp_local_ns;

    Snapshot() : exchange(Exchange::UNKNOWN), sequence(0), timestamp_exchange_ns(0), timestamp_local_ns(0) {}
};

// Convert Exchange to string
inline const char* exchange_to_string(Exchange ex) {
    switch (ex) {
        case Exchange::BINANCE: return "BINANCE";
        case Exchange::OKX: return "OKX";
        case Exchange::COINBASE: return "COINBASE";
        case Exchange::KRAKEN: return "KRAKEN";
        default: return "UNKNOWN";
    }
}

// Convert Side to string
inline const char* side_to_string(Side side) {
    return side == Side::BID ? "BID" : "ASK";
}

}  // namespace trading
