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
