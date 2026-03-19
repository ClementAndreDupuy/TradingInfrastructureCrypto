# Coinbase market-data protocol choice

This connector standardizes on Coinbase Advanced Trade market-data WebSocket subscriptions using `level2` for subscribe requests and `l2_data` for received book messages.

## Endpoints

| Purpose | URL |
|---|---|
| WebSocket | `wss://advanced-trade-ws.coinbase.com` |
| Tick-size REST (public) | `https://api.coinbase.com/api/v3/brokerage/market/products/{product_id}` |

The legacy Exchange API (`api.exchange.coinbase.com`) only supports USD-denominated spot pairs and must **not** be used here: the Advanced Trade WebSocket uses USDT pairs (e.g. `SOL-USDT`) that do not exist on the Exchange API, causing `400` responses.

## Subscription contract

- The handler sends one `subscribe` request for `heartbeats` and one for `level2`, matching Coinbase's current one-channel-per-message contract.
- If `COINBASE_API_KEY` / `COINBASE_API_SECRET` or the existing `LIVE_` / `SHADOW_` variants are present, each subscribe payload includes a freshly generated ES256 JWT using Coinbase's ES256 JWT flow. **A fresh JWT must be generated per message** as each token expires after 120 s.
- The heartbeat subscribe is sent without `product_ids`; the `level2` subscribe carries the mapped product id.
- Unauthenticated access is permitted for `level2` market data but authenticated connections are more reliable per Coinbase documentation.

## Why `l2_data`

- Provides snapshot + incremental updates with sequence numbers.
- Supports full-price-level depth updates needed by our normalized `Snapshot` / `Delta` pipeline.
- Matches project invariants across feeds: deterministic ordering, sequence continuity, and explicit re-sync on gap.

## Invariants enforced by `CoinbaseFeedHandler`

1. The handler only normalizes `channel == "l2_data"` book messages.
2. A valid `snapshot` event must be processed before streaming deltas.
3. Sequence continuity is strict: each delta must have `sequence_num == last + 1`.
4. Any sequence gap triggers forced re-sync (`State::BUFFERING` + error callback).
5. Heartbeat messages refresh liveness state and stale-heartbeat detection forces a reconnect/resync.
6. Snapshot and deltas propagate Coinbase exchange timestamps into project-level `Snapshot` / `Delta` types when the venue supplies them.

## Full-depth fidelity target

Coinbase emits level2 depth updates by price level. We preserve all provided updates and rely on sequence integrity plus deterministic apply order to maintain depth fidelity in the shared order book state.
