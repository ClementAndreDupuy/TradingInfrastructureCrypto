# CLAUDE.md

Guidance for Claude Code in this repository.

## System Overview

Microsecond-latency crypto trading system targeting Binance, OKX, and Coinbase.

**Architecture split:**
- **C++ hot path** — order book, execution, risk (latency-critical, no Python)
- **Python cold path** — research, backtesting, monitoring (no latency constraint). Goal: feed a research `.md` file → Claude implements the logic, backtests, approves, and deploys the alpha, reducing TTM.

## Build Commands

```bash
# C++ (no CMake)
./scripts/build.sh
./scripts/run_binance_example.sh

# C++ (CMake)
mkdir -p build && cd build && cmake .. && make -j$(nproc)

# Python
pip install -e .
pytest tests/unit/

# pybind11 bridge
pip3 install pybind11
python3 bindings/setup.py build_ext --inplace

# Backtest (C++ engine)
python3 research/backtest/fetch_and_run.py --ticks 60 --interval-ms 500 --engine cpp
```

## Key Principles

1. C++ owns the hot path — Python never touches live execution or risk.
2. Order book integrity is foundational — all downstream depends on book correctness.
3. Shadow = live code path — identical code before going live.
4. Kill switch is non-negotiable — must work at OS/hardware level.
5. Never mock data — always use real implementations.
6. Avoid too many comments in the code, keep it clean and short 
7. Always write production ready code, so we can iterate on a working env 
8. Avoid creating too many files, more compact is more readable

## Critical Components

- **`core/orderbook/`** — Flat-array book keyed by price tick grid (O(1), cache-local). Silent divergence poisons all decisions.
- **`core/feeds/`** — Per-exchange WebSocket handlers (Binance, Kraken). Sequence validation + gap detection.
- **`core/risk/`** — Pre-trade checks, kill switch, circuit breaker. Sub-microsecond, no Python.
- **`core/execution/`** — Order manager + exchange connectors.
- **`core/shadow/`** — Paper trading with identical code path to live. Run ≥ 2 weeks before live.
- **`bindings/`** — pybind11 bridge: exposes `OrderBook` + `ArbRiskManager` + `PerpArbSim` to Python.
- **`research/backtest/`** — Event-driven backtest. Uses C++ matching engine via pybind11.

## Technology Stack

- **C++17** — Boost.Asio, libwebsockets, ZeroMQ, pybind11
- **Python 3.10+** — Polars (not Pandas), NumPy, statsmodels, pytest
- **Storage** — Arctic, kdb+, Parquet
- **Monitoring** — Prometheus, Grafana

## Directory Structure

```
core/          C++ hot path
  orderbook/   Book structure
  feeds/       Exchange WebSocket handlers
  ipc/         Shared memory, ring buffers
  risk/        Pre-trade checks, kill switch
  execution/   Order manager, connectors
  shadow/      Shadow trading engine
  strategy/    Strategy implementations

research/      Python cold path
  data/        Storage connectors
  features/    Signal engineering
  alpha/       Alpha models
  backtest/    Backtest engine

bindings/      pybind11 C++↔Python bridge
config/        dev / shadow / live configs
tests/         unit / integration / replay
```

## Code Style

**C++** — `snake_case` files/functions/members, `PascalCase` classes, `UPPER_SNAKE_CASE` constants, trailing `_` on private members. C++17, no exceptions/RTTI in hot path, `#pragma once`.

**Python** — `snake_case` files/functions, `PascalCase` classes, type hints required, Google-style docstrings, `black` + `ruff`, Polars not Pandas.

## Performance Budgets

| Operation | Budget |
|---|---|
| Order book delta | < 1 µs |
| Risk check | < 1 µs |
| Feed handler | < 10 µs |
| Backtest (1 month tick) | < 10 min |

## Security

- Credentials in `config/live/secrets.yaml` (gitignored). Never hardcode.
- Use separate API keys per environment (dev / shadow / live).
- Rotate keys quarterly.

## Testing Checklist

**Before committing:**
- [ ] All unit tests pass (`make test` + `pytest tests/unit/`)
- [ ] Linters clean (`clang-format`, `black`, `ruff`)
- [ ] No performance regression on hot path

**Before shadow:**
- [ ] Full replay test passes
- [ ] No memory leaks (valgrind)
- [ ] Latency budgets met

**Before live:**
- [ ] Shadow running ≥ 2 weeks
- [ ] Shadow fill rates match backtest within 10%
- [ ] Kill switch drill completed
- [ ] All monitoring dashboards operational

## Development Workflow

### Adding a new alpha signal
1. Research in `research/notebooks/` with Polars
2. Feature engineering in `research/features/`
3. IC/ICIR analysis in `research/alpha/`
4. Backtest via `research/backtest/` using C++ engine
5. Walk-forward validation (crypto regimes change frequently)

### Adding a new exchange
1. Feed handler in `core/feeds/{exchange}/`
2. Snapshot/delta protocol + sequence validation + gap detection
3. Integration test with recorded feed data

### Common pitfalls
- `std::map` in order book (use flat arrays)
- Backtesting at mid-price (replay actual bid/ask)
- Skipping shadow trading phase
- Python in the hot path
- Missing sequence number validation
