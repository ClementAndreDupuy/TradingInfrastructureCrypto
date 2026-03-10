# core/feeds/

Per-exchange WebSocket feed handlers. Each exchange has different protocols.

## Exchange-Specific Quirks

### Binance (`binance/`)
- **Snapshot**: REST call to `/depth?symbol=X&limit=1000`
- **Deltas**: WebSocket stream `wss://stream.binance.com:9443/ws/{symbol}@depth@100ms`
- **Sequence**: `lastUpdateId` in snapshot, `U` (first) and `u` (last) in deltas
- **Gap handling**: If `U <= lastUpdateId + 1 <= u`, apply delta
- **Quirk**: Sends buffered updates every 100ms, not real-time ticks

### Kraken (`kraken/`)
- **Snapshot**: REST `GET /0/public/Depth?pair={symbol}&count=500`
  - Response: `{"error":[],"result":{"XXBTZUSD":{"bids":[["price","vol",ts],...], "asks":[...]}}}`
  - **Quirk**: Response key uses internal pair name (e.g. `XXBTZUSD`) regardless of request; parse by finding `"bids"` key, not by pair name
  - **No sequence number**: REST response has no `lastUpdateId`; `last_seq` starts at 0
- **Deltas**: WebSocket v2 `wss://ws.kraken.com/v2`, channel `book`
  - Subscribe: `{"method":"subscribe","params":{"channel":"book","symbol":["BTC/USD"],"depth":10}}`
  - Snapshot msg: `{"channel":"book","type":"snapshot","seq":1,"data":[{"bids":[{"price":X,"qty":Y}...],"asks":[...]}]}`
  - Update msg: `{"channel":"book","type":"update","seq":N,"data":[...]}`
  - Price levels: `{"price":X,"qty":Y}` objects (not arrays like Binance)
- **Sequence**: Top-level `"seq"` field, strictly +1 per message (no Binance-style `U <= lastId+1 <= u` window)
- **Gap handling**: `seq != last_seq + 1` → trigger re-snapshot
- **Checksum**: `"checksum"` field (CRC32) present in every update; not currently validated

### OKX (`okx/`)
- **Snapshot**: WebSocket `books` channel with `action: snapshot`
- **Deltas**: Same channel with `action: update`
- **Sequence**: No explicit sequence number, timestamp-based ordering
- **Quirk**: Checksum provided - MUST validate `checksum` field against computed CRC32

### Coinbase (`coinbase/`)
- **Protocol**: Full L3 book (order-level, not price-level)
- **Snapshot**: REST `/products/{id}/book?level=3`
- **Deltas**: WebSocket `full` channel (`open`, `done`, `match`, `change` messages)
- **Sequence**: Explicit `sequence` field, strictly incrementing
- **Quirk**: High message rate, need efficient order ID tracking

## Implementation Checklist

For each exchange handler:

1. **Connection Management**
   - Reconnect on disconnect with exponential backoff
   - Handle ping/pong to keep connection alive
   - Rate limit reconnection attempts

2. **Snapshot/Delta Synchronization**
   - Take snapshot
   - Buffer deltas during snapshot
   - Apply buffered deltas after snapshot
   - Validate sequence continuity

3. **Error Handling**
   - Log all WebSocket errors
   - Detect stale data (no update for N seconds)
   - Trigger re-snapshot on corruption

4. **Timestamping**
   - Record exchange timestamp (from message)
   - Record local receipt timestamp (hardware, PTP)
   - Tag with sequence number

## Testing

- **Recorded data replay** - Store real WebSocket traffic, replay through handler
- **Synthetic gap injection** - Drop messages, verify re-snapshot trigger
- **Checksum validation** (OKX) - Ensure every update passes checksum
- **Latency measurement** - Track delta between exchange timestamp and local processing

## Common Issues

- **Buffering during snapshot** - Forgetting to buffer deltas while fetching snapshot
- **Time zone confusion** - Exchange timestamps in different zones (UTC vs local)
- **WebSocket keepalive** - Connection silently dies without ping/pong
- **Rate limits** - Snapshot requests rate-limited, need backoff
