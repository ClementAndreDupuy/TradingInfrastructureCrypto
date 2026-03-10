# core/execution/

Order manager and exchange connectors. Low-latency order submission and tracking.

## Order States

`NEW → SUBMITTED → PENDING_NEW → ACTIVE → FILLED` or `REJECTED / CANCELED`.

## Responsibilities

- Order lifecycle management (submission → fill → cancel)
- Exchange connectivity (WebSocket for Binance/OKX, FIX for Coinbase)
- Partial fill tracking with VWAP
- Reconciliation after reconnect

## Performance Targets

- Submit latency (local): < 100 µs
- Fill processing: < 1 µs

## Exchange Protocols

- **Binance** — Orders via REST, fill updates via WebSocket user-data stream.
- **OKX** — Orders and fill updates via WebSocket private channel.
- **Coinbase** — FIX 4.2 `NewOrderSingle (D)` / `ExecutionReport (8)`.

## Error Handling

Never auto-retry rejections — let the strategy decide. On disconnect, reconcile open orders before resuming.
