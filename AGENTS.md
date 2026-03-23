# AGENTS.md

Guidance for agents in this repository (compatible with Codex AGENTS.md and Claude Code).

Note: `AGENTS.md` is the canonical source. `CLAUDE.md` files are mirrored for Claude Code compatibility.

## System Overview

Microsecond-latency crypto trading system targeting Binance, Kraken, OKX, and Coinbase.

**Architecture split:**
- **C++ hot path** — order book, execution, risk (latency-critical, no Python)
- **Python cold path** — research, backtesting, monitoring (no latency constraint). Goal: feed a research `.md` file → Claude implements the logic, backtests, approves, and deploys the alpha, reducing TTM.

## Build Commands

```bash
# C++ (CMake)
mkdir -p build && cd build && cmake .. && make -j$(nproc)

# Python
pip install -e .
pytest tests/unit/

# pybind11 bridge
pip3 install pybind11
python3 core/bindings/setup.py build_ext --inplace

# Neural alpha pipeline (live data)
python -m research.neural_alpha.pipeline --exchanges KRAKEN --ticks 300 --epochs 20

# Neural alpha pipeline (synthetic, for testing)
python -m research.neural_alpha.pipeline --synthetic --ticks 400 --epochs 5
```

## Perform an audit of the codebase
Audit guidelines of the project

[Audit guidelines](/docs/AUDIT.md)

## Development Guidelines
Development guidelines of the project.

[Development guidelines](/docs/DEVELOPMENT_GUIDELINES.md)

## Project TODOs
All project todos should be placed in the TODO file below, ranked by priority and severity. The execution-engine rebuild is complete, so keep any follow-up execution work tracked directly in `/docs/TODOS.md` and update this `AGENTS.md` in the same commit whenever execution-operating guidance changes.

[TODOS file](/docs/TODOS.md)

## Components

- **`core/orderbook/`** — Flat-array book keyed by price tick grid (O(1), cache-local).
- **`core/feeds/`** — Per-exchange WebSocket handlers (Binance, Kraken, OKX, Coinbase) with snapshot/delta sync, sequence validation, and reconnect backoff.
- **`core/risk/`** — Pre-trade checks, kill switch, circuit breaker. Sub-microsecond, no Python.
- **`core/execution/`** — `ExchangeConnector` interface + live connectors (Binance/Kraken/OKX/Coinbase), `OrderManager` (position tracking), `NeuralAlphaMarketMaker` (GTX limit orders, signal skew, stop-limit stop-loss).
- **`core/ipc/`** — Shared memory bridge: Python publishes neural alpha signal → C++ reads via `AlphaSignalReader`.
- **`core/shadow/`** — `ShadowConnector` + `ShadowEngine` — paper trading with identical code path to live. Run ≥ 2 weeks before live.
- **`research/neural_alpha/`** — Primary GNN+Transformer alpha model + secondary compact ensemble model, walk-forward training, backtest, shadow session.
- **`research/regime/`** — Regime HMM that publishes `calm` / `trending` / `shock` / `illiquid` probabilities for the live stack.

## Technology Stack

- **C++17** — Boost.Asio, libwebsockets, pybind11
- **Python 3.10+** — Polars (not Pandas), NumPy, PyTorch, pytest
- **Storage** — Parquet (primary), Arctic/kdb+ (optional)
- **Monitoring** — Prometheus, Grafana

## Directory Structure

```
core/            C++ hot path
  common/        Shared types (Order, FillUpdate, OrderType, etc.)
  orderbook/     Book structure
  feeds/         Exchange WebSocket handlers (Binance/Kraken/OKX/Coinbase)
  ipc/           Shared memory (neural alpha signal bridge)
  risk/          Pre-trade checks, kill switch
  execution/     Market maker, order manager, exchange connector
  shadow/        Shadow trading engine
  bindings/      pybind11 C++↔Python bridge

research/        Python cold path
  neural_alpha/  Primary + secondary alpha models, features, backtest, shadow session
  regime/        HMM regime model + IPC publisher
  backtest/      Shared backtest utilities (shadow_metrics)

config/          dev / shadow / live runtime + risk configs
deploy/          AWS infrastructure, systemd services, operational scripts
  scripts/       Build and preflight scripts
docs/            Audit reports, development guidelines, TODOs
tests/           unit / integration / replay / perf
```

## Code Style

**C++** — `snake_case` files/functions/members, `PascalCase` classes, `UPPER_SNAKE_CASE` constants, trailing `_` on private members. C++17, no exceptions/RTTI in hot path, `#pragma once`.

**Python** — `snake_case` files/functions, `PascalCase` classes, type hints required, Polars not Pandas.

## Performance Budgets

| Operation | Budget |
|---|---|
| Order book delta | < 1 µs |
| Risk check | < 1 µs |
| Feed handler | < 10 µs |
| Backtest (1 month tick) | < 10 min |

## Security

- Credentials in `config/live/secrets.yaml` (gitignored). Never hardcode.
- Separate API keys per environment (dev / shadow / live).

## Reusable Agent Memory (Updated)

Use this as a lightweight operating checklist for future agent sessions.

1. **Always discover nested instructions first**: run `rg --files -g 'AGENTS.md'` before editing code.
2. **Current top-level layout includes additional active areas** beyond the high-level tree above:
   - `core/engine/` (engine orchestration surface)
   - `tests/perf/` (performance-focused checks)
   - `deploy/` (AWS + systemd deployment assets)
3. **Typical safe verification flow after edits**:
   - Python: `pytest tests/unit/`
   - C++ build/test: `mkdir -p build && cd build && cmake .. && make -j$(nproc)` then `cd build && make test`
4. **Agent change hygiene**:
   - Keep hot-path and cold-path boundaries explicit in commits.
   - Update scoped `AGENTS.md` whenever new conventions are introduced.
   - Prefer replay/integration validation whenever touching book/feed/risk paths.

5. **Execution follow-up hygiene**:
   - Track any new execution-engine follow-up items directly in `docs/TODOS.md` with concrete acceptance criteria.
   - Update this `AGENTS.md` in the same commit whenever execution-operating guidance changes.

## Known Issues Fixed

### Execution-engine roadmap retirement (2026-03-23)
- **`docs/TODOS.md`** + **`AGENTS.md`** — The phased execution-engine roadmap has been retired because the rebuild is complete, and any new execution follow-up now belongs in the shared prioritized TODO list instead of a separate roadmap file.

### Phase 7 adaptive venue quality completion (2026-03-23)
- **`tests/unit/venue_quality_model_test.cpp`** — Phase 7 now includes a noisy-input scheduler regression that verifies adaptive venue scoring stays stable and continues to prefer the better venue under short-term execution noise.
- **`research/backtest/shadow_metrics.py`** + **`tests/unit/test_shadow_metrics.py`** — Shadow reports now summarize adaptive venue-priority changes, including fill probability, passive/taker markout, reject rate, cancel latency, and explicit reasons each venue improved or degraded over the run.

### Phase 7 adaptive venue quality groundwork (2026-03-23)
- **`core/execution/common/quality/venue_quality_model.hpp`** + **`core/execution/router/smart_order_router.[hpp/cpp]`** — Routing now applies bounded adaptive venue penalties derived from rolling fill probability, markout, reject rate, cancel latency, and venue health so scheduler decisions can respond to changing venue quality without oscillating on every noisy sample.
- **`core/engine/trading_engine_main.cpp`** — The engine now emits periodic `venue quality` snapshots for post-trade analysis, which Phase 7 completion reporting now explains in shadow metrics output.

### Execution-engine phases 0-6 complete (2026-03-23)
- **`core/engine/trading_engine_main.cpp`** — The execution-engine rebuild reached adaptive venue quality modelling after Phases 0 through 6 completed, so future work starts from incremental execution follow-up rather than roadmap migration work.

### Phase 6 live target-position cutover (2026-03-23)
- **`core/engine/trading_engine_main.cpp`** — Live execution now consumes cached reconciliation snapshots and feeds the same target-position loop used in shadow with actual live inventory.

### Phase 5 shadow state machine rollout (2026-03-23)
- **`core/engine/trading_engine_main.cpp`** + **`core/shadow/shadow_engine.hpp`** — Shadow execution now runs only the new target-position engine in shadow mode and logs testable `STATE_TRANSITION` events for `FLAT`/`ENTERING`/`HOLDING`/`REDUCING`/`FLATTENING`/`HALTED`.
- **`research/backtest/shadow_metrics.py`** — Shadow reporting now includes state-transition counts alongside churn, shortfall, and realized edge capture metrics for the new engine path.

### Execution connector remediation (2026-03-19)
- **`core/execution/okx/okx_connector.cpp`** + **`core/execution/live_connector_base.hpp`** — OKX private REST auth now sends the documented passphrase header, spot/swap order placement includes venue-correct `tdMode` and client IDs, cancel/query/amend requests are instrument-scoped with `instId` plus `ordId`/`clOrdId`, and `cancel_all()` is now explicitly unsupported instead of calling `cancel-batch-orders` with an undocumented payload.
- **`core/execution/binance/binance_connector.cpp`** — Binance Spot signed requests now use query-string `timestamp`/`signature`, order placement supplies venue-correct mandatory fields plus `newClientOrderId`, order query/cancel/fill reconciliation are symbol-scoped, and replace flows use `POST /api/v3/order/cancelReplace` with explicit rejection of unsupported stop-limit semantics.
- **`core/execution/kraken/kraken_connector.cpp`** + **`core/execution/live_connector_base.hpp`** — Kraken private REST calls now sign the form-encoded payload with the documented nonce + URI-path contract using the base64-decoded secret, AddOrder includes limit price and `cl_ord_id`, replace flows use `POST /0/private/AmendOrder`, and `cancel_all()` now targets `POST /0/private/CancelAll` instead of the dead-man-switch endpoint.
- **`core/execution/coinbase/coinbase_connector.cpp`** + **`core/execution/live_connector_base.hpp`** — Coinbase Advanced Trade private REST calls now use per-request bearer JWT authentication with the documented CDP key + EC private key flow, create/edit/query/cancel/account/fill requests match the current `/api/v3/brokerage` contracts, futures positions reconcile through `/cfm/positions`, and `cancel_all()` is explicitly rejected because Advanced Trade does not expose a product-scoped cancel-all contract compatible with the internal interface.

### Shadow session (2026-03-18)
- **`research/neural_alpha/features.py`** — spread normalisation divided by zero when bid == ask. Guard changed to `raw_spread > 0` instead of `mid > 0`.
- **`research/neural_alpha/shadow_session.py`** — `train_on_recent` raised `RuntimeError` when all walk-forward folds were skipped (dataset smaller than `seq_len`). Now logs a warning and returns; continuous training retries on next tick accumulation.
- **`core/engine/trading_engine_main.cpp`** — In shadow mode, connectors target `mock://` URLs so `fetch_reconciliation_snapshot` REST calls always fail, immediately quarantining every venue. Reconciliation connect/periodic-drift cycle is now skipped when `mode == "shadow"`.
- **`core/feeds/coinbase/coinbase_feed_handler.cpp`** — A failed snapshot (`ERROR_BOOK_CORRUPTED`) left the handler in `BUFFERING` state with no re-subscribe, causing `start()` to hang until its 30 s timeout. `trigger_resnapshot` now sets `reconnect_requested_` which forces the WS event loop to close and reconnect.
