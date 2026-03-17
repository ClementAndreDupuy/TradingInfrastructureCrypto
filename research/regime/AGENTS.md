# research/regime/

Real-time market regime identification for the R2 research component.

## Responsibility

Classifies the current market microstructure into one of four semantic regimes:
`calm`, `trending`, `shock`, `illiquid`. Probabilities are published to a
shared-memory IPC file read by the C++ hot path (`core/ipc/regime_signal.hpp`).

## Components

- `regime.py` — all regime logic:
  - `RegimeConfig` / `RegimeArtifact` — config and serialisable model artifact
  - `train_regime_model_from_ipc()` — k-means training from LOB snapshots
  - `infer_regime_probabilities()` — soft probability assignment for one tick
  - `save_regime_artifact()` / `load_regime_artifact()` — JSON persistence
  - `RegimeSignalPublisher` — seqlock-based mmap writer (`/tmp/regime_signal.bin`)

## IPC Contract

Binary layout (48 bytes, little-endian):

| Offset | Size | Field             |
|--------|------|-------------------|
| 0      | 8    | seq (uint64)      |
| 8      | 8    | p_calm (double)   |
| 16     | 8    | p_trending (double)|
| 24     | 8    | p_shock (double)  |
| 32     | 8    | p_illiquid (double)|
| 40     | 8    | ts_ns (int64, system clock) |

The C++ reader (`RegimeSignalReader`) uses seqlock: reads are valid only when
`seq` is even and matches before/after the payload copy.

## Rules

1. Never import from `core/` — IPC is the only bridge.
2. `RegimeSignalPublisher.close()` must NOT delete the signal file; the C++
   reader holds a persistent mmap and a deletion window during model reloads
   would cause fallback to conservative defaults.
3. Python's `time.time_ns()` produces UNIX wall-clock nanoseconds — the C++
   staleness check must use `std::chrono::system_clock`, not `steady_clock`.
4. `_semantic_regime_names` must never produce duplicate labels; use
   priority-based assignment that skips already-named clusters.
