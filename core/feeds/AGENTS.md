# core/feeds/

Per-exchange market data connectors and shared book state.

## Layout

- `binance/` — Binance spot depth snapshot+delta handler.
- `kraken/` — Kraken spot book handler.
- `okx/` — OKX public book handler.
- `coinbase/` — Coinbase Advanced Trade level2 handler.
- `common/` — shared feed utilities (`book_manager.hpp`, `tick_size.hpp`).

Keep this layout: one folder per exchange and shared code in `common/`.

## Connector responsibilities

Each feed connector must:

1. Call `fetch_tick_size()` synchronously at the start of `start()`, before spawning the WebSocket thread.
2. Fetch or derive an initial snapshot.
3. Buffer deltas while synchronizing.
4. Validate sequence continuity.
5. Apply deltas in order.
6. Trigger resync on gaps/staleness/checksum failures.
7. Expose exchange + local timestamps in produced events.

## Tick size selection

Each handler queries the exchange's public symbol-info endpoint to determine the price grid resolution used by the `OrderBook`. The fetched value is exposed via `tick_size()` so callers can construct the `OrderBook` with matching precision:

```cpp
handler.start();
OrderBook book(symbol, exchange, handler.tick_size(), 20000);
```

Exchange endpoints and fields:

| Exchange | Endpoint | Field |
|----------|----------|-------|
| Binance  | `GET /api/v3/exchangeInfo?symbol=X` | `PRICE_FILTER.tickSize` (string) |
| Kraken   | `GET /0/public/AssetPairs?pair=X`   | `pair_decimals` (int) → `10^-n` |
| OKX      | `GET /api/v5/public/instruments`    | `data[0].tickSz` (string) |
| Coinbase | `GET /products/{product_id}`        | `quote_increment` (string) |

String-formatted tick sizes (Binance, OKX, Coinbase) are parsed via `tick_from_string()` (`common/tick_size.hpp`), which counts significant decimal digits and returns `10^-n` exactly — avoiding the floating-point noise that `stod("0.01000000")` introduces in `price_to_index()`. `fetch_tick_size()` failures are non-fatal: `tick_size_` stays `0.0`, a WARN is logged, and the `OrderBook`'s non-positive-base guard catches any bad snapshot.

## Integration contract with execution / SOR

Feed handlers publish normalized `Snapshot` / `Delta` updates into the shared `BookManager` state. Execution and smart routing components must not depend on exchange-specific wire formats.

When modifying feed handlers, preserve deterministic ordering and sequence integrity so `SmartOrderRouter` inputs remain consistent across venues.

## Common pitfalls

- Applying deltas before snapshot synchronization completes.
- Accepting out-of-order sequence numbers.
- Treating reconnect as a continuation instead of requiring re-sync.
- Constructing `OrderBook` with a hardcoded tick size instead of reading `handler.tick_size()`.

## Agent memory

- Feed code stays exchange-isolated (`<exchange>/`) with normalization in `common/` only.
- When adding a venue handler, document: snapshot source, sequence/checksum rule, exact re-sync trigger conditions, and the symbol-info endpoint + field used for tick size.
- Reconnect is a correctness boundary: treat as fresh sync unless the venue protocol explicitly guarantees continuity.
- Keep comments limited to venue protocol contracts, sequencing rules, and resync triggers.
