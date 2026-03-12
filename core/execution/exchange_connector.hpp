#pragma once

// ExchangeConnector — abstract interface for submitting and cancelling orders.
//
// Both live connectors and ShadowConnector implement this interface, which lets
// the strategy layer be identical for paper and live trading.
//
// Thread model:
//   submit_order / cancel_order / cancel_all are called from the strategy thread.
//   on_fill is dispatched from the same thread (synchronously in shadow mode,
//   or from a WebSocket/FIX thread in live mode — caller must serialise).

#include "../common/types.hpp"
#include <functional>

namespace trading {

class ExchangeConnector {
public:
    virtual ~ExchangeConnector() = default;

    virtual Exchange        exchange_id()  const = 0;
    virtual bool            is_connected() const = 0;

    virtual ConnectorResult connect()    = 0;
    virtual void            disconnect() = 0;

    virtual ConnectorResult submit_order(const Order& order)        = 0;
    virtual ConnectorResult cancel_order(uint64_t client_order_id)  = 0;
    virtual ConnectorResult cancel_all(const char* symbol)          = 0;

    // Reconcile local order state with exchange state (called on reconnect).
    virtual ConnectorResult reconcile() = 0;

    // Fill/cancel/reject notifications — set by the order manager.
    std::function<void(const FillUpdate&)> on_fill;

protected:
    void emit_fill(const FillUpdate& u) {
        if (on_fill) on_fill(u);
    }
};

}  // namespace trading
