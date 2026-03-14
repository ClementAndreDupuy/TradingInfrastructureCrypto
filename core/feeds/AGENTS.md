# core/feeds/

Per-exchange WebSocket feed handlers. Each exchange has a different protocol.

## Exchange Quirks

**Binance** — Snapshot via REST `/depth`. Deltas via WebSocket `@depth@100ms`. Gap detection: `U <= lastUpdateId+1 <= u`. Updates buffered every 100 ms.

**Kraken** — Snapshot via REST `/0/public/Depth` (response key is internal pair name, e.g. `XXBTZUSD`). Deltas via WebSocket v2, channel `book`. Sequence strictly `+1` per message.

**OKX** — WebSocket `books` channel. Checksum validation (CRC32) is required on every update.

**Coinbase** — Full L3 book (order-level). FIX protocol. Strictly incrementing sequence.

## Implementation Checklist

For each exchange handler: reconnect with backoff, ping/pong keepalive, snapshot/delta sync (buffer deltas during snapshot), sequence continuity validation, stale-data detection, dual timestamps (exchange + local PTP).

## Common Issues

- Forgetting to buffer deltas while fetching snapshot
- Exchange timestamps in different timezones
- Silent WebSocket death without ping/pong
- Snapshot request rate limits requiring backoff
