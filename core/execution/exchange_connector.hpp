#pragma once

// Abstract exchange connector interface and order types.
//
// Concrete implementations: BinanceConnector, KrakenConnector
// Thread safety: submit_order/cancel_order from strategy thread;
//                fill_callback_ called from WS receive thread.

#include "../common/types.hpp"
#include "../common/logging.hpp"
#include <string>
#include <functional>
#include <cstdint>
#include <cstring>
#include <atomic>

namespace trading {

// ── Order types ───────────────────────────────────────────────────────────────

enum class OrderType : uint8_t { LIMIT = 0, MARKET = 1 };

enum class TimeInForce : uint8_t {
    GTC = 0,  // Good Till Canceled
    IOC = 1,  // Immediate Or Cancel
    FOK = 2,  // Fill Or Kill
};

enum class OrderState : uint8_t {
    NEW         = 0,
    SUBMITTED   = 1,
    PENDING_NEW = 2,
    ACTIVE      = 3,
    FILLED      = 4,
    CANCELED    = 5,
    REJECTED    = 6,
};

enum class ConnectorResult : uint8_t {
    OK              = 0,
    ERROR_NETWORK   = 1,
    ERROR_AUTH      = 2,
    ERROR_REJECTED  = 3,
    ERROR_DUPLICATE = 4,
    ERROR_UNKNOWN   = 5,
};

// Fixed-size order struct — no heap allocation.
struct Order {
    uint64_t     client_order_id;
    char         exchange_order_id[32];   // Exchange-assigned ID
    char         symbol[16];              // e.g. "BTCUSDT" or "BTC/USD"
    Exchange     exchange;
    Side         side;
    OrderType    type;
    TimeInForce  tif;
    double       price;
    double       quantity;
    double       filled_qty;
    double       avg_fill_price;
    OrderState   state;
    int64_t      submit_ts_ns;
    int64_t      last_update_ts_ns;

    Order() noexcept { std::memset(this, 0, sizeof(Order)); }
};

// Fill/status notification delivered via callback.
struct FillUpdate {
    uint64_t   client_order_id;
    char       exchange_order_id[32];
    double     fill_price;                // Last fill price (0 if no fill this update)
    double     fill_qty;                  // Last fill quantity
    double     cumulative_filled_qty;     // Total filled so far
    double     avg_fill_price;            // Exchange-reported VWAP (0 if not provided)
    OrderState new_state;
    int64_t    exchange_ts_ns;
    int64_t    local_ts_ns;
    char       reject_reason[64];         // Non-empty on REJECTED

    FillUpdate() noexcept {
        std::memset(this, 0, sizeof(FillUpdate));
        new_state = OrderState::ACTIVE;
    }
};

using FillCallback  = std::function<void(const FillUpdate&)>;
using ErrorCallback = std::function<void(const std::string&)>;

inline const char* order_state_to_string(OrderState s) noexcept {
    switch (s) {
        case OrderState::NEW:         return "NEW";
        case OrderState::SUBMITTED:   return "SUBMITTED";
        case OrderState::PENDING_NEW: return "PENDING_NEW";
        case OrderState::ACTIVE:      return "ACTIVE";
        case OrderState::FILLED:      return "FILLED";
        case OrderState::CANCELED:    return "CANCELED";
        case OrderState::REJECTED:    return "REJECTED";
        default:                      return "UNKNOWN";
    }
}

// ── Abstract connector ────────────────────────────────────────────────────────

class ExchangeConnector {
public:
    virtual ~ExchangeConnector() = default;
    ExchangeConnector(const ExchangeConnector&) = delete;
    ExchangeConnector& operator=(const ExchangeConnector&) = delete;

    void set_fill_callback(FillCallback cb)  { fill_callback_  = std::move(cb); }
    void set_error_callback(ErrorCallback cb) { error_callback_ = std::move(cb); }

    // Connect and start the private order-update stream.
    virtual ConnectorResult connect() = 0;
    virtual void            disconnect() = 0;

    // Submit a new order. Confirmation arrives asynchronously via fill_callback_.
    virtual ConnectorResult submit_order(const Order& order) = 0;

    // Cancel by client_order_id. Confirmation via fill_callback_.
    virtual ConnectorResult cancel_order(uint64_t client_order_id) = 0;

    // Emergency: cancel all open orders for a symbol.
    virtual ConnectorResult cancel_all(const char* symbol) = 0;

    // Fetch open orders from exchange and reconcile local state.
    virtual ConnectorResult reconcile() = 0;

    virtual bool     is_connected() const = 0;
    virtual Exchange exchange_id()  const = 0;

protected:
    ExchangeConnector() = default;

    FillCallback  fill_callback_;
    ErrorCallback error_callback_;

    void emit_fill(const FillUpdate& u)      { if (fill_callback_)  fill_callback_(u); }
    void emit_error(const std::string& msg)  { if (error_callback_) error_callback_(msg); }
};

}  // namespace trading
