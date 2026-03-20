# research/

Python cold path for research, validation, and runtime signal publication.

## Rules

1. Keep `research/` isolated from the C++ hot path except for documented IPC files.
2. Use Polars for tabular work and keep type hints on public Python APIs.
3. Preserve walk-forward validation and point-in-time safety in every data or modeling change.
4. Prefer narrow modules with one responsibility and keep orchestration in package entrypoints.

## Folder Architecture

- `neural_alpha/data/` — feature engineering and dataset/window assembly
- `neural_alpha/models/` — neural network definitions and training loops
- `neural_alpha/evaluation/` — backtests and alpha-quality analytics
- `neural_alpha/operations/` — data-quality gates and model-governance utilities
- `neural_alpha/runtime/` — IPC bridge and live shadow-session runtime
- `neural_alpha/pipeline.py` — end-to-end orchestration entrypoint
- `regime/` — regime modeling and IPC publication
- `backtest/` — shared backtest utilities

## Reusable Agent Memory

- Prefer editing the domain package that owns the behavior before touching `pipeline.py`.
- Keep compatibility wrappers under `research/neural_alpha/` lightweight when cross-repo imports still depend on legacy module paths.
- Update tests when files move so imports exercise the new package layout directly.
- Remove dead helpers and stale research artifacts when refactors make them obsolete.
