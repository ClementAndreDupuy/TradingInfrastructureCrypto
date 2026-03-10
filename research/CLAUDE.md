# research/

**Python cold path** — no latency constraint. Research, backtesting, monitoring.

## Critical Rules

1. Never call from C++ hot path — this layer is async, accessed via IPC.
2. Use Polars, not Pandas — 10-100x faster for tick data.
3. Walk-forward validation — crypto regimes change; use rolling windows.
4. Store everything — disk is cheap, missing data is expensive.

## Components

- **data/** — Storage connectors (Arctic, kdb+, Parquet)
- **features/** — Signal/feature engineering
- **alpha/** — Alpha models, IC/ICIR analysis
- **backtest/** — Event-driven backtest engine (uses C++ matching via pybind11)
- **notebooks/** — Exploratory only, never production logic

## Data Schema

Every tick record: `symbol`, `exchange`, `timestamp_exchange` (ns), `timestamp_local` (ns), `sequence`, `bid_prices[]`, `bid_sizes[]`, `ask_prices[]`, `ask_sizes[]`.

## Common Pitfalls

- Using Pandas — too slow for tick data
- Vectorized backtest — unrealistic fill assumptions
- In-sample overfitting — always walk-forward
- Ignoring latency — signal at T, execution at T+latency
- Mid-price backtesting — use actual bid/ask
