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

## Neural Alpha Improvements TODO

Quick wins (low effort, high payoff):

- [ ] **Direction-head gating** (`shadow_session.py`) — signal is currently `ret[1] * 1e4` only; gate it by requiring `direction_probs[up or down] > 0.55` to agree before publishing a non-zero signal
- [ ] **More OFI lags** (`features.py`) — add OFI at lags 5, 10, 20 ticks in addition to the current lag-1; `D_SCALAR` goes from 10 → 13

Medium effort:

- [ ] **Per-level queue imbalance** (`features.py`) — for each of the 5 LOB levels add `(bid_size_i - ask_size_i) / (bid_size_i + ask_size_i + 1e-8)`; adds 5 scalar features
- [ ] **Wider volatility windows + vol ratio** (`features.py`) — add `vol_60`, `vol_200`, and `vol_5 / vol_60`; the ratio implicitly captures regime (trending vs ranging)
- [ ] **Better adverse selection label** (`features.py`) — replace spread-widening proxy with price reversion after fill: adverse = mid moves more than half-spread against direction within 10 ticks

Higher effort:

- [ ] **Rolling normalization** (`dataset.py`) — replace fold-level z-score stats with a 500-tick rolling window so the model sees deviation from recent history, not the whole fold
- [ ] **1-tick return head** (`model.py`) — add horizon 1 to `RETURN_HORIZONS = [1, 10, 100, 500]`; use it in `shadow_session.py` as an entry gate: skip if `ret[0]` is strongly negative even when `ret[2]` is positive
- [ ] **Ensemble two models** (`pipeline.py`) — train a second smaller model (`d_spatial=32, n_temp_layers=1`) with a different seed; average signals to reduce variance and improve ICIR by ~20–30%
