# R4 Research — Smart Order Routing (SOR) for Binance, Kraken, OKX, Coinbase

Date: 2026-03-14
Scope: Execution-layer research and implementation plan for multi-venue smart routing in the C++ hot path.

## 1) Objective

Design and implement a **production-grade Smart Order Router** in `core/execution/` that:

1. Routes child orders across **Binance, Kraken, OKX, and Coinbase** using real-time depth, fees, and venue health.
2. Preserves the project hot-path constraints (C++ only, no Python in live execution).
3. Directly unblocks:
   - **C9**: live exchange connectors with auth, retries, idempotency, and order-id mapping.
   - **C10**: real `trading_engine` binary wiring feeds + books + risk + execution in live mode.

## 2) Current gap analysis (project-specific)

### What already exists
- A generic `ExchangeConnector` interface (`submit_order`, `cancel_order`, `cancel_all`, `reconcile`) is defined.
- `OrderManager` and `NeuralAlphaMarketMaker` are present and strategy-facing.
- Feed handlers exist for Binance/Kraken/OKX/Coinbase, so market-data plumbing is mostly available.

### What is missing for SOR
- No concrete live connector implementations per exchange in `core/execution/`.
- No cross-venue routing policy/scoring component.
- No executable entrypoint that builds a full live execution graph.
- Deployment scripts/services still show single-venue assumptions in places.

## 3) Target design

## 3.1 Core components (new)

1. **`VenueConnector` implementations (C9 foundation)**
   - `binance_connector.[hpp/cpp]`
   - `kraken_connector.[hpp/cpp]`
   - `okx_connector.[hpp/cpp]`
   - `coinbase_connector.[hpp/cpp]`
   - All implement `ExchangeConnector` and expose uniform behavior for:
     - HMAC signing/auth headers.
     - Client idempotency key support.
     - Exchange↔client order id mapping.
     - Retry/backoff with bounded attempt policy.
     - `reconcile()` for restart/reconnect state recovery.

2. **SOR policy engine**
   - `smart_order_router.[hpp/cpp]`
   - Responsibilities:
     - Venue eligibility filtering (connectivity, stale book, min size, symbol support).
     - Per-venue **effective price** computation:
       - top-of-book + sweep/slippage estimate from local depth.
       - maker/taker fee + expected rebates.
       - expected latency penalty.
       - risk penalty (venue instability, recent rejects, stale sequence).
     - Child-order split policy:
       - single-venue aggressive route for urgent IOC marketable size.
       - multi-venue split for larger notional to minimize implementation shortfall.

3. **Routing config and fee model**
   - `config/{dev,shadow,live}/routing.yaml`
   - Includes:
     - per-venue maker/taker bps, estimated withdrawal/custody overhead (optional).
     - max participation rate / clip size / minimum clip.
     - latency budget per venue.
     - failover thresholds (reject ratio, stale-book threshold, heartbeat timeout).

4. **Execution gateway wiring (C10 foundation)**
   - `core/engine/trading_engine_main.cpp` (or `core/main.cpp`)
   - Constructs:
     - feed handlers + book manager per venue.
     - risk + circuit breaker.
     - connectors.
     - SOR + order manager + strategy.
   - Exposes command-line `--mode live|shadow --venues BINANCE,KRAKEN,OKX,COINBASE --symbol BTCUSDT`.

## 3.2 Routing algorithm (practical + low-latency)

Use a deterministic scoring model (no heap alloc on hot path):

For each venue `v`, for side `s` and quantity `q`:

`score_v = px_impact_v(q, s) + fee_v(q, s) + latency_penalty_v + risk_penalty_v`

Route to venue(s) with minimum `score_v`, subject to constraints.

Implementation notes:
- Compute `px_impact_v` from in-memory order-book levels (already streamed).
- If required depth is unavailable, mark venue ineligible for that clip.
- Maintain venue health ring-buffer counters:
  - rejects/acks/fills time-window
  - ws lag
  - REST failures
- Convert health to `risk_penalty_v` and eventually hard-disable a venue until cooldown.

## 3.3 Order lifecycle and idempotency model

For every parent order:
1. Generate `parent_id` (monotonic local).
2. SOR emits one or multiple child orders with unique `client_order_id` per venue.
3. Connector sends venue-native request with idempotency key format:
   - `TRT-{parent_id}-{child_seq}-{venue}`
4. On ack, persist mapping:
   - `(client_order_id -> venue_order_id, venue, symbol)`
5. `reconcile()` on startup/reconnect:
   - query open orders and positions per venue.
   - rebuild mapping table and re-emit synthetic fills/cancels if needed.

This is required to avoid duplicate orders on retries and to support cancel/recover semantics.

## 4) Exchange-specific implementation requirements

## 4.1 Binance
- REST signed endpoints: timestamp + recvWindow + HMAC SHA256 signature.
- Prefer POST new order + DELETE cancel with clientOrderId when available.
- Handle `-1013`/filter failures (lot size, tick size, min notional) as non-retryable.

## 4.2 Kraken
- Private REST auth with API-Key + API-Sign over path + nonce + payload digest.
- Distinguish temporary engine/network errors vs invalid order params.
- Support post-only / time-in-force mapping carefully.

## 4.3 OKX
- Auth headers include key, passphrase, timestamp, sign.
- Ensure instrument id translation (project symbol ↔ OKX instId).
- Handle exchange-side precision and reduce-only semantics when applicable.

## 4.4 Coinbase
- Advanced Trade auth/signing and product-id mapping.
- Respect rate-limit headers and perform bounded retry with jitter.
- Normalize order status transitions to common `OrderState`.

## 5) How this solves C9 and C10

### C9 (connectors)
This plan implements all required live connector features:
- submit/cancel/cancel_all/reconcile
- authenticated signing
- retries + backoff
- idempotency keys
- exchange order-id mapping

### C10 (real trading engine executable)
This plan introduces a concrete `trading_engine` binary and `main()` that wires:
- feeds + books + risk + execution + SOR
- full four-venue runtime configuration
- systemd service compatibility (`deploy/systemd/trading-engine.service`)

## 6) Rollout plan (production-safe)

1. **Phase A — Connector primitives**
   - Build four connectors with contract tests for state machine transitions.
   - Add per-venue sandbox/dev smoke scripts.

2. **Phase B — Router in shadow mode**
   - Enable SOR with `ShadowConnector` parity path.
   - Replay historical multi-venue books and verify deterministic routing outputs.

3. **Phase C — Live canary**
   - Start with one symbol and hard notional cap.
   - Progressive venue enablement:
     - Binance + Kraken first, then OKX, then Coinbase.

4. **Phase D — Full deployment**
   - Enable four-venue orchestration in `deploy/run_live.sh` and production service.
   - Enforce kill-switch and breaker gates before submit path.

## 7) Required tests (must pass before live)

1. **Connector contract tests** (per exchange)
   - submit ack path
   - partial fill then cancel
   - reject mapping
   - reconnect + reconcile idempotency

2. **Router deterministic replay tests**
   - Given recorded books, verify same child split + venue choice byte-for-byte.

3. **Chaos tests**
   - venue disconnect, stale feed, 429/rate-limit, high reject burst.
   - expected behavior: automatic de-prioritization / failover.

4. **Risk gate integration tests**
   - stale-book and drawdown guards must block SOR submits.

5. **Performance tests**
   - routing decision latency budget: target < 5 µs/decision in hot path.

## 8) Concrete implementation backlog (next code tasks)

1. Add `core/execution/venue_order_map.hpp` fixed-capacity table (no heap on hot path).
2. Add four connector classes and unit tests under `tests/unit/`.
3. Add `core/execution/smart_order_router.hpp` + `cpp` with score-based routing.
4. Add `core/engine/trading_engine_main.cpp` and CMake target `trading_engine`.
5. Update `deploy/systemd/trading-engine.service` and `deploy/run_live.sh` for four-venue launch.
6. Add config parser for `routing.yaml`.
7. Add replay + connector contract tests in CI.

## 9) Risk notes and controls

- **Primary risk**: inconsistent symbol/precision rules across venues causing rejects.
  - Control: per-venue symbol metadata cache + pre-submit quantization.
- **Primary risk**: duplicate orders after network retries.
  - Control: strict idempotency key + reconcile ledger.
- **Primary risk**: stale or divergent market data influences routing.
  - Control: stale-book guards, sequence continuity checks, forced venue cooldown.

## 10) Recommendation

Proceed with R4 implementation immediately as the dependency chain for live readiness.
R4 is a direct enabler for C9/C10 and also reduces execution slippage/cost once multi-venue routing is active.
