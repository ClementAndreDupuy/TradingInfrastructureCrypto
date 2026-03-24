# core/

**C++ hot path** — all latency-critical code lives here.

## Rules

1. No Python — never call Python from this layer.
2. No heap allocation in hot path — pre-allocate everything at startup.
3. Cache-friendly structures — flat arrays over `std::map`.
4. PTP timestamps — never `std::chrono::system_clock`.
5. Fail fast — log and halt on bad state, never silently continue.

## Components

- **common/** — Shared types: `Order`, `FillUpdate`, `OrderType`, `TimeInForce`, `OrderState`, `ConnectorResult`, `Exchange`, `Side`
- **orderbook/** — Flat-array O(1) book; all downstream depends on its correctness
- **feeds/** — Per-exchange WebSocket handlers (Binance, Kraken, OKX, Coinbase) with snapshot/delta sync, continuity checks, and reconnect backoff
- **ipc/** — `AlphaSignalReader`: mmaps `/tmp/neural_alpha_signal.bin` written by Python shadow session
- **risk/** — Pre-trade checks, kill switch (sub-µs, lock-free)
- **execution/** — `ExchangeConnector` (interface), live venue connectors (Binance/Kraken/OKX/Coinbase), `OrderManager` (position + fills), `NeuralAlphaMarketMaker` (GTX quotes, alpha skew, stop-limit)
- **shadow/** — `ShadowConnector` + `ShadowEngine`: paper trading with identical code path to live

## Performance Requirements

- Order book update: < 1 µs
- Risk check: < 1 µs
- Feed handler: < 10 µs end-to-end

## Reusable Agent Memory (Updated)

- Keep `core/` edits tightly scoped: avoid cross-cutting refactors that blend feed/orderbook/risk changes in one commit.
- Current directory map under `core/` includes: `common/`, `engine/`, `execution/`, `feeds/`, `ipc/`, `orderbook/`, `risk/`, `shadow/`.
- For changes that influence trading decisions, validate in this order:
  1. `tests/unit/`
  2. `tests/integration/` (feed→book→risk path)
  3. `tests/replay/` for sequence/regression confidence
- If a change could impact latency, add or run `tests/perf/` checks before merging.
- Keep comments in `core/` sparse and high-signal: remove banner comments, usage walkthroughs, and line-by-line restatements of obvious code; keep only invariants, concurrency/memory-ordering notes, protocol contracts, and non-obvious safety rationale.


## Core Classes & Methods (Quick Reference)

### `orderbook/`
- **`OrderBook`** — Owns the in-memory L2 grid and applies snapshots/deltas atomically with sequence checks.
  - `apply_snapshot(const Snapshot&)`: Rebuilds the grid from a fresh snapshot and validates spread/checksum.
  - `apply_delta(const Delta&)`: Applies one price-level update and recenters grid on repeated out-of-range updates.
  - `get_best_bid()/get_best_ask()/get_mid_price()`: Returns current top-of-book prices used by execution/risk.

### `feeds/`
- **`BinanceFeedHandler` / `KrakenFeedHandler` / `OkxFeedHandler` / `CoinbaseFeedHandler`** — Venue-specific WS+snapshot synchronizers.
  - `start()/stop()`: Starts or stops sync + WS loop lifecycle.
  - `process_message(const std::string&)`: Parses wire payloads and emits normalized snapshot/delta callbacks.
  - `refresh_tick_size()`: Loads venue tick-size metadata before book construction.
- **`BookManager`** — Feed→OrderBook adapter with freshness tracking and optional IPC LOB publishing.
  - `snapshot_handler()/delta_handler()`: Returns callbacks that apply updates into `OrderBook`.
  - `age_ms()`: Returns staleness age for circuit-breaker checks.

### `ipc/`
- **`AlphaSignalReader`** — Seqlock mmap reader for neural alpha/risk signals from Python.
  - `open()/close()/read()`: Manages mapped file and returns last consistent signal frame.
  - `allows_long()/allows_short()/allows_mm()`: Applies stale/fail-open gating for strategy permissions.
- **`RegimeSignalReader`** — Reads regime probabilities (`calm/trending/shock/illiquid`) from mmap.
  - `read()`: Returns a consistent regime frame with retry-on-writer logic.
  - `is_stale(...)`: Flags stale regime frames for safety fallbacks.
- **`LobPublisher`** — Single-writer mmap ring for publishing normalized top-of-book snapshots.
  - `open()/close()`: Creates/maps shared memory file and initializes ring header.
  - `publish(...)`: Writes one top-5 LOB frame into the next ring slot.

### `risk/`
- **`CircuitBreaker`** — Fast runtime guards for rates, drawdown, stale books, and flash-crash detection.
  - `check_order_rate()/check_message_rate()`: Enforces per-second/per-minute throughput caps.
  - `check_drawdown()/check_price_deviation()/check_book_age(...)`: Trips kill switch on loss/staleness/shock.
  - `record_leg_result(...)`: Tracks consecutive-loss streaks and halts on breach.
- **`GlobalRiskControls`** — Notional/concentration/venue-cap envelope checks and commits.
  - `check_order(...)`: Simulates post-trade risk state and returns breach reason.
  - `commit_order(...)`: Atomically commits exposure state and triggers kill-switch on breach.
- **`RecoveryGuard`** — Guards recovery path anomaly counters (in-flight ops/duplicate acks/races).
  - `check(...)`: Validates counters against limits and halts when breached.
- **`RiskConfigLoader`** — Lightweight loader for risk runtime config YAML-like key/value files.
  - `load(path, out)`: Parses risk fields and applies aliases/defaults into runtime config.

### `execution/`
- **`ExchangeConnector`** — Abstract venue connector contract.
  - `submit_order/cancel_order/replace_order/query_order/cancel_all/reconcile`: Required venue operations.
- **`LiveConnectorBase`** — Shared live-venue implementation (idempotency, retries, order-id map).
  - `submit_order()/cancel_order()/replace_order()`: Wraps venue calls with recovery journal semantics.
  - `fetch_reconciliation_snapshot(...)`: Extension point for venue drift snapshots.
- **`OrderManager`** — Strategy-thread order lifecycle + position ledger with lock-free fill queue.
  - `submit()/cancel()/cancel_all()`: Places/cancels orders through connector.
  - `drain_fills()`: Applies queued fills, updates state/PnL, fires fill callbacks.
- **`NeuralAlphaMarketMaker`** — Quote engine with alpha/regime skewing and stop-limit flattening.
  - `on_book_update()`: Main strategy tick: reads signals, updates quotes, evaluates stops.
  - `set_alpha_signal(...)`: Test hook for injected alpha/risk values.
- **`SmartOrderRouter`** — Venue scoring/splitting logic for child-order routing.
  - `route()/route_with_alpha(...)`: Builds child orders from venue quote set and constraints.
  - `score_venue_bps()/expected_shortfall_bps(...)`: Computes normalized routing costs.
