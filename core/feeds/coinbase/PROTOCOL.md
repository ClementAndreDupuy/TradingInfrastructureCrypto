# Coinbase market-data protocol choice

This connector standardizes on Coinbase Advanced Trade `l2_data` over WebSocket.

## Why `l2_data`

- Provides snapshot + incremental updates with sequence numbers.
- Supports full-price-level depth updates needed by our normalized `Snapshot` / `Delta` pipeline.
- Matches project invariants across feeds: deterministic ordering, sequence continuity, and explicit re-sync on gap.

## Invariants enforced by `CoinbaseFeedHandler`

1. The handler only accepts `channel == "l2_data"` messages.
2. A valid `snapshot` event must be processed before streaming deltas.
3. Sequence continuity is strict: each delta must have `sequence_num == last + 1`.
4. Any sequence gap triggers forced re-sync (`State::BUFFERING` + error callback).
5. Snapshot and deltas are normalized into project-level `Snapshot`/`Delta` types consumed by `BookManager`.

## Full-depth fidelity target

Coinbase emits level2 depth updates by price level. We preserve all provided updates and rely on sequence integrity plus deterministic apply order to maintain depth fidelity in the shared order book state.
