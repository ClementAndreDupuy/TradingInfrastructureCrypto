#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace trading {
    enum class Exchange : uint8_t { BINANCE = 0, OKX = 1, COINBASE = 2, KRAKEN = 3, UNKNOWN = 255 };

    enum class Side : uint8_t { BID = 0, ASK = 1 };

    enum class Result : uint8_t {
        SUCCESS = 0,
        ERROR_INVALID_SEQUENCE = 1,
        ERROR_INVALID_PRICE = 2,
        ERROR_INVALID_SIZE = 3,
        ERROR_SEQUENCE_GAP = 4,
        ERROR_BOOK_CORRUPTED = 5,
        ERROR_CONNECTION_LOST = 6
    };

    struct PriceLevel {
        double price;
        double size;

        PriceLevel() : price(0.0), size(0.0) {
        }

        PriceLevel(double p, double s) : price(p), size(s) {
        }
    };

    struct Delta {
        Side side;
        double price;
        double size;
        uint64_t sequence;
        int64_t timestamp_exchange_ns;
        int64_t timestamp_local_ns;

        Delta()
            : side(Side::BID), price(0.0), size(0.0), sequence(0), timestamp_exchange_ns(0),
              timestamp_local_ns(0) {
        }
    };

    struct Snapshot {
        std::string symbol;
        Exchange exchange;
        uint64_t sequence;
        std::vector<PriceLevel> bids;
        std::vector<PriceLevel> asks;
        uint32_t checksum;
        bool checksum_present;
        int64_t timestamp_exchange_ns;
        int64_t timestamp_local_ns;

        Snapshot()
            : exchange(Exchange::UNKNOWN), sequence(0), checksum(0), checksum_present(false),
              timestamp_exchange_ns(0), timestamp_local_ns(0) {
        }
    };

    inline const char *exchange_to_string(Exchange ex) {
        switch (ex) {
            case Exchange::BINANCE:
                return "BINANCE";
            case Exchange::OKX:
                return "OKX";
            case Exchange::COINBASE:
                return "COINBASE";
            case Exchange::KRAKEN:
                return "KRAKEN";
            default:
                return "UNKNOWN";
        }
    }

    inline const char *side_to_string(Side side) { return side == Side::BID ? "BID" : "ASK"; }

    enum class OrderType : uint8_t {
        MARKET = 0,
        LIMIT = 1,
        STOP_LIMIT = 2,
    };

    enum class TimeInForce : uint8_t {
        GTC = 0,
        IOC = 1,
        FOK = 2,
        GTX = 3,
    };

    enum class OrderState : uint8_t {
        PENDING = 0,
        OPEN = 1,
        PARTIALLY_FILLED = 2,
        FILLED = 3,
        CANCELED = 4,
        REJECTED = 5,
    };

    enum class ConnectorResult : uint8_t {
        OK = 0,
        ERROR_RATE_LIMIT = 1,
        ERROR_INSUFFICIENT_FUNDS = 2,
        ERROR_INVALID_ORDER = 3,
        ERROR_REST_FAILURE = 4,
        AUTH_FAILED = 5,
        ERROR_UNKNOWN = 255,
    };

    struct Order {
        uint64_t client_order_id = 0;
        char symbol[16] = {};
        Exchange exchange = Exchange::UNKNOWN;
        Side side = Side::BID;
        OrderType type = OrderType::LIMIT;
        TimeInForce tif = TimeInForce::GTC;
        double price = 0.0;
        double stop_price = 0.0;
        double quantity = 0.0;
        int64_t submit_ts_ns = 0;
    };

    struct FillUpdate {
        uint64_t client_order_id = 0;
        double fill_price = 0.0;
        double fill_qty = 0.0;
        double cumulative_filled_qty = 0.0;
        double avg_fill_price = 0.0;
        OrderState new_state = OrderState::PENDING;
        int64_t local_ts_ns = 0;
        char reject_reason[64] = {};
    };
} // namespace trading
