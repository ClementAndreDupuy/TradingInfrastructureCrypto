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
All the project todos should be placed in the TODO file below, ranked by priority and severity.

[TODOS file](/docs/TODOS.md)

## Components

- **`core/orderbook/`** — Flat-array book keyed by price tick grid (O(1), cache-local).
- **`core/feeds/`** — Per-exchange WebSocket handlers (Binance, Kraken, OKX, Coinbase) with snapshot/delta sync, sequence validation, and reconnect backoff.
- **`core/risk/`** — Pre-trade checks, kill switch, circuit breaker. Sub-microsecond, no Python.
- **`core/execution/`** — `ExchangeConnector` interface + live connectors (Binance/Kraken/OKX/Coinbase), `OrderManager` (position tracking), `NeuralAlphaMarketMaker` (GTX limit orders, signal skew, stop-limit stop-loss).
- **`core/ipc/`** — Shared memory bridge: Python publishes neural alpha signal → C++ reads via `AlphaSignalReader`.
- **`core/shadow/`** — `ShadowConnector` + `ShadowEngine` — paper trading with identical code path to live. Run ≥ 2 weeks before live.
- **`research/neural_alpha/`** — GNN spatial + Transformer temporal model, walk-forward training, backtest, shadow session.

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
  neural_alpha/  GNN+Transformer model, features, backtest, shadow session
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
