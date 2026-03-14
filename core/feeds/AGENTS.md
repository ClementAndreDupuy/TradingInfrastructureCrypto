# core/feeds/

Per-exchange WebSocket feed handlers. Each exchange has a different protocol.

## Exchange Quirks

**Binance** — Snapshot via REST `/depth`. Deltas via WebSocket `@depth@100ms`. Gap detection: `U <= lastUpdateId+1 <= u`. Updates buffered every 100 ms.

**Kraken** — Snapshot via REST `/0/public/Depth` (response key is internal pair name, e.g. `XXBTZUSD`). Deltas via WebSocket v2, channel `book`. Sequence strictly `+1` per message.

**OKX** — WebSocket `books` channel with REST snapshot + delta sync and sequence continuity checks. CRC32 checksum verification is still TODO and should force re-sync on mismatch.

**Coinbase** — Advanced Trade `level2` WebSocket channel (price-level depth). Maintain strict sequence continuity and consistent snapshot/delta merge semantics.

## Implementation Checklist

For each exchange handler: reconnect with exponential backoff, ping/pong keepalive, snapshot/delta sync (buffer deltas during snapshot), sequence continuity validation, stale-data detection, snapshot rate-limit handling, dual timestamps (exchange + local PTP).

## Common Issues

- Forgetting to buffer deltas while fetching snapshot
- Exchange timestamps in different timezones
- Silent WebSocket death without ping/pong
- Snapshot request rate limits requiring backoff
