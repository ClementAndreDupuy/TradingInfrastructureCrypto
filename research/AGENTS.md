# research/

**Python cold path** — no latency constraint. Research, backtesting, monitoring.

## Rules

1. Never call from C++ hot path — this layer is async, accessed via IPC.
2. Use Polars, not Pandas.
3. Walk-forward validation — crypto regimes change; use rolling windows.
4. Store everything — disk is cheap, missing data is expensive.

## Components

- **alpha/neural_alpha/** — Dual-model alpha stack: primary GNN spatial + Transformer temporal model plus a smaller secondary ensemble model
  - `model.py` — `CryptoAlphaNet`: `LOBSpatialEncoder` + `TemporalEncoder` + multi-task heads
  - `features.py` — LOB tensor + 13 scalar features (OFI at lags 1/5/10/20, vol, spread, etc.)
  - `dataset.py` — sliding-window `LOBDataset`, walk-forward splits
  - `trainer.py` — contrastive pre-training + supervised walk-forward training
  - `pipeline.py` — end-to-end: fetch → train primary + secondary → backtest → alpha report → save both alpha artifacts + regime artifact
  - `backtest.py` — event-driven backtest with realistic fills (bid/ask, taker fee, stop-loss)
  - `shadow_session.py` — live inference, direction-head gating, requires primary + secondary alpha artifacts and regime artifact in production mode, publishes to `/tmp/neural_alpha_signal.bin`
- **regime/** — HMM regime model training/inference + IPC publication for the live research stack
- **backtest/** — Shared backtest utilities
- **notebooks/** — Exploratory only, never production logic

## Data Schema

Every tick record: `symbol`, `exchange`, `timestamp_ns`, `bid_price_{1-5}`, `bid_size_{1-5}`, `ask_price_{1-5}`, `ask_size_{1-5}`.

## Neural Alpha — Execution

The real execution engine is **C++**: `core/execution/market_maker.hpp`.
Python has no execution logic — research and signal generation only.

Signal bridge: `shadow_session.py` → `/tmp/neural_alpha_signal.bin` → `core/ipc/alpha_signal.hpp` → `NeuralAlphaMarketMaker`.

## Neural Alpha Improvements TODO

All listed neural-alpha improvement items in this file have now been implemented in the codebase; remove stale TODO entries here and track any new follow-up work in `docs/TODOS.md`.

## Reusable Agent Memory (Updated)

- Treat `pipeline.py` as orchestration glue: feature/model/backtest changes should be implemented in their owning modules first.
- Maintain Python typing discipline (function signatures + return types) to support automated refactors.
- Prefer synthetic pipeline runs for quick smoke validation, then live-exchange runs for realism checks.
- Preserve IPC contract compatibility with `core/ipc/alpha_signal.hpp` when modifying signal publishing fields.
- Research production mode now assumes **three artifacts** are present: primary alpha model, secondary alpha model, and regime model. Do not silently drop to single-model mode in production-oriented changes.
