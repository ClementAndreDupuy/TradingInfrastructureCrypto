# core/execution/

Order manager and exchange connectors. **Low-latency order submission and tracking.**

## Responsibilities

1. **Order lifecycle management** - Track orders from submission to fill/cancel
2. **Exchange connectivity** - WebSocket (Binance/OKX) and FIX (Coinbase)
3. **Partial fill tracking** - Maintain open order state
4. **Fill/cancel confirmations** - Match exchange responses to local orders

## Order States

```
NEW → SUBMITTED → PENDING_NEW → ACTIVE → FILLED
                           ↓              ↓
                      REJECTED      CANCELED
```

## Key Components (to be implemented)

- `order_manager.hpp/cpp` - Central order tracking
- `exchange_connector.hpp` - Abstract interface for all exchanges
- `binance_connector.cpp` - Binance WebSocket implementation
- `okx_connector.cpp` - OKX WebSocket implementation
- `coinbase_connector.cpp` - Coinbase FIX implementation

## Order Submission Flow

```cpp
// 1. Create order
Order order{
    .symbol = "BTC-USD",
    .side = Side::BUY,
    .type = OrderType::LIMIT,
    .price = 50000.0,
    .quantity = 0.1,
    .client_order_id = generate_client_id()
};

// 2. Risk check (in risk/ layer)
if (!risk_check(order)) {
    return RiskRejection;
}

// 3. Submit to exchange
auto result = connector->submit_order(order);

// 4. Track locally
order_manager.add_pending_order(order);

// 5. Handle async response
// (exchange sends confirmation via WebSocket/FIX)
```

## Handling Partial Fills

```cpp
void on_fill_update(const FillUpdate& update) {
    auto* order = order_manager.get_order(update.order_id);

    order->filled_qty += update.fill_qty;
    order->avg_fill_price = update_vwap(order, update);

    if (order->filled_qty >= order->quantity) {
        order->state = OrderState::FILLED;
    }

    // Publish fill to IPC for PnL calculation
    publish_fill(update);
}
```

## Exchange-Specific Protocols

### Binance
- WebSocket: `wss://stream.binance.com:9443/ws`
- Order submit: REST POST `/api/v3/order`
- Updates: User data stream (requires listenKey)

### OKX
- WebSocket: `wss://ws.okx.com:8443/ws/v5/private`
- Order submit: WebSocket `order` channel
- Updates: Same WebSocket, `orders` channel

### Coinbase
- Protocol: FIX 4.2
- Order submit: `NewOrderSingle (D)`
- Updates: `ExecutionReport (8)`

## Latency Optimization

1. **Connection pooling** - Pre-established WebSocket connections
2. **Zero-copy messaging** - Serialize directly to send buffer
3. **Batch cancels** - Cancel multiple orders in single message when possible
4. **Avoid REST** - Use WebSocket for all order operations (except Binance, where REST is required)

## Error Handling

```cpp
// Exchange rejects order
void on_order_reject(const Rejection& rej) {
    log_error("Order rejected: {}", rej.reason);
    order_manager.mark_rejected(rej.order_id);
    // DO NOT retry automatically - let strategy decide
}

// Lost connection
void on_disconnect() {
    log_error("Exchange connection lost");
    // Request order status for all open orders
    reconcile_open_orders();
    // Reconnect with backoff
}
```

## Order Reconciliation

After disconnect/reconnect, reconcile local state with exchange:
```cpp
void reconcile_open_orders() {
    auto local_orders = order_manager.get_open_orders();
    auto exchange_orders = connector->fetch_open_orders();

    // Find discrepancies
    for (auto& local_order : local_orders) {
        if (!exchange_orders.contains(local_order.id)) {
            // Order filled/canceled while disconnected
            request_order_status(local_order.id);
        }
    }
}
```

## Testing

1. **Mock exchange** - Simulate order submission/fills without real exchange
2. **Replay fills** - Replay historical fill data through order manager
3. **Disconnect scenarios** - Simulate connection loss, verify reconciliation
4. **Race conditions** - Submit/cancel in quick succession

## Performance Targets

- **Submit latency**: < 100 microseconds (local processing)
- **Network latency**: 1-10 milliseconds (to exchange, depends on co-location)
- **Fill processing**: < 1 microsecond
