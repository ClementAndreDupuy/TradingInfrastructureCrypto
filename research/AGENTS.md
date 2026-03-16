# research/

**Python cold path** — no latency constraint. Research, backtesting, monitoring.

## Rules

1. Never call from C++ hot path — this layer is async, accessed via IPC.
2. Use Polars, not Pandas.
3. Walk-forward validation — crypto regimes change; use rolling windows.
4. Store everything — disk is cheap, missing data is expensive.

## Components

- **alpha/neural_alpha/** — GNN spatial + Transformer temporal model
  - `model.py` — `CryptoAlphaNet`: `LOBSpatialEncoder` + `TemporalEncoder` + multi-task heads
  - `features.py` — LOB tensor + 13 scalar features (OFI at lags 1/5/10/20, vol, spread, etc.)
  - `dataset.py` — sliding-window `LOBDataset`, walk-forward splits
  - `trainer.py` — contrastive pre-training + supervised walk-forward training
  - `pipeline.py` — end-to-end: fetch → train → backtest → alpha report
  - `backtest.py` — event-driven backtest with realistic fills (bid/ask, taker fee, stop-loss)
  - `shadow_session.py` — live inference, direction-head gating, publishes signal to `/tmp/neural_alpha_signal.bin`
- **backtest/** — Shared backtest utilities
- **notebooks/** — Exploratory only, never production logic

## Data Schema

Every tick record: `symbol`, `exchange`, `timestamp_ns`, `bid_price_{1-5}`, `bid_size_{1-5}`, `ask_price_{1-5}`, `ask_size_{1-5}`.

## Neural Alpha — Execution

The real execution engine is **C++**: `core/execution/market_maker.hpp`.
Python has no execution logic — research and signal generation only.

Signal bridge: `shadow_session.py` → `/tmp/neural_alpha_signal.bin` → `core/ipc/alpha_signal.hpp` → `NeuralAlphaMarketMaker`.

## Neural Alpha Improvements TODO

Quick wins:

- [x] **Direction-head gating** (`shadow_session.py`) — gates signal by `direction_probs > 0.55`
- [x] **More OFI lags** (`features.py`) — OFI at lags 5, 10, 20; `D_SCALAR` = 13

Medium effort:

- [ ] **Per-level queue imbalance** (`features.py`) — `(bid_size_i - ask_size_i) / (bid_size_i + ask_size_i + 1e-8)` for each LOB level; adds 5 scalar features
- [ ] **Wider volatility windows + vol ratio** (`features.py`) — add `vol_60`, `vol_200`, `vol_5 / vol_60`
- [x] **Better adverse selection label** (`features.py`) — replaced spread-widening proxy with price reversion after fill within 10 ticks

Higher effort:

- [ ] **Rolling normalization** (`dataset.py`) — 500-tick rolling z-score instead of fold-level
- [ ] **1-tick return head** (`model.py`) — add horizon 1; use in `shadow_session.py` as entry gate
- [ ] **Ensemble two models** (`pipeline.py`) — second smaller model (`d_spatial=32, n_temp_layers=1`), average signals for ~20–30% ICIR improvement

## Reusable Agent Memory (Updated)

- Treat `pipeline.py` as orchestration glue: feature/model/backtest changes should be implemented in their owning modules first.
- Maintain Python typing discipline (function signatures + return types) to support automated refactors.
- Prefer synthetic pipeline runs for quick smoke validation, then live-exchange runs for realism checks.
- Preserve IPC contract compatibility with `core/ipc/alpha_signal.hpp` when modifying signal publishing fields.
