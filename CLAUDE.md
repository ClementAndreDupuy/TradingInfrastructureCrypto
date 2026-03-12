# CLAUDE.md

Guidance for Claude Code in this repository.

## System Overview

Microsecond-latency crypto trading system targeting Binance, OKX, and Coinbase.

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
python3 bindings/setup.py build_ext --inplace

# Neural alpha pipeline (live data)
python -m research.alpha.neural_alpha.pipeline --exchanges KRAKEN --ticks 300 --epochs 20

# Neural alpha pipeline (synthetic, for testing)
python -m research.alpha.neural_alpha.pipeline --synthetic --ticks 400 --epochs 5
```

## Key Principles

1. C++ owns the hot path — Python never touches live execution or risk.
2. Order book integrity is foundational — all downstream depends on book correctness.
3. Shadow = live code path — identical code before going live.
4. Kill switch is non-negotiable — must work at OS/hardware level.
5. Never mock data — always use real data and real implementation.
6. Keep code clean and short — avoid unnecessary comments.
7. Always write production-ready code.
8. Run a full audit of the repo every week to identify bugs, grade the compared to industry standard and propose improvements
9. Always be 100% honest in your review

## Components

- **`core/orderbook/`** — Flat-array book keyed by price tick grid (O(1), cache-local).
- **`core/feeds/`** — Per-exchange WebSocket handlers (Binance, Kraken). Sequence validation + gap detection.
- **`core/risk/`** — Pre-trade checks, kill switch, circuit breaker. Sub-microsecond, no Python.
- **`core/execution/`** — `ExchangeConnector` interface, `OrderManager` (position tracking), `NeuralAlphaMarketMaker` (GTX limit orders, signal skew, stop-limit stop-loss).
- **`core/ipc/`** — Shared memory bridge: Python publishes neural alpha signal → C++ reads via `AlphaSignalReader`.
- **`core/shadow/`** — `ShadowConnector` + `ShadowEngine` — paper trading with identical code path to live. Run ≥ 2 weeks before live.
- **`research/alpha/neural_alpha/`** — GNN spatial + Transformer temporal model, walk-forward training, backtest, shadow session.

## Technology Stack

- **C++17** — Boost.Asio, libwebsockets, pybind11
- **Python 3.10+** — Polars (not Pandas), NumPy, PyTorch, pytest
- **Storage** — Parquet (primary), Arctic/kdb+ (optional)
- **Monitoring** — Prometheus, Grafana

## Directory Structure

```
core/          C++ hot path
  common/      Shared types (Order, FillUpdate, OrderType, etc.)
  orderbook/   Book structure
  feeds/       Exchange WebSocket handlers
  ipc/         Shared memory (neural alpha signal bridge)
  risk/        Pre-trade checks, kill switch
  execution/   Market maker, order manager, exchange connector
  shadow/      Shadow trading engine

research/      Python cold path
  alpha/
    neural_alpha/   GNN+Transformer model, features, backtest, shadow session
  backtest/    Event-driven backtest engine

bindings/      pybind11 C++↔Python bridge
config/        dev / shadow / live configs
tests/         unit / integration / replay
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

## Pre-Commit Checklist

- [ ] Unit tests pass (`make test` + `pytest tests/unit/`)
- [ ] Linters clean (`clang-format`, `black`, `ruff`)
- [ ] No performance regression on hot path

## Before Live

- [ ] Shadow running ≥ 2 weeks
- [ ] Shadow fill rates match backtest within 10%
- [ ] Kill switch drill completed
- [ ] All monitoring dashboards operational
