# core/feeds/

Per-exchange market data connectors and shared book state.

## Layout

- `binance/` — Binance spot depth snapshot+delta handler.
- `kraken/` — Kraken spot book handler.
- `okx/` — OKX public book handler.
- `coinbase/` — Coinbase Advanced Trade level2 handler.
- `common/` — shared feed utilities (`book_manager.hpp`).

Keep this layout: one folder per exchange and shared code in `common/`.

## Connector responsibilities

Each feed connector should:

1. Fetch or derive an initial snapshot.
2. Buffer deltas while synchronizing.
3. Validate sequence continuity.
4. Apply deltas in order.
5. Trigger resync on gaps/staleness/checksum failures.
6. Expose exchange + local timestamps in produced events.

## Integration contract with execution / SOR

Feed handlers publish normalized `Snapshot` / `Delta` updates into the shared
`BookManager` state. Execution and smart routing components consume this
normalized depth view; they must not depend on exchange-specific wire formats.

When modifying feed handlers, preserve deterministic ordering and sequence
integrity so `SmartOrderRouter` inputs remain consistent across venues.

## Common pitfalls

- Applying deltas before snapshot synchronization completes.
- Accepting out-of-order sequence numbers.
- Treating reconnect as a continuation instead of requiring re-sync.
- Mixing exchange-specific precision rules without normalization.

## Reusable Agent Memory (Updated)

- Feed code should remain exchange-isolated (`<exchange>/`) with normalization only in shared/common paths.
- When adding/changing a venue handler, document:
  1. snapshot source + cadence
  2. sequence/checksum continuity rule
  3. exact re-sync trigger conditions
- Reconnect handling is a correctness boundary: treat reconnect as fresh sync unless venue protocol guarantees continuity.
