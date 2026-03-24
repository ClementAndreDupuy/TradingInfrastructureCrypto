# ETHUSDT / SOLUSDT Shadow Log Deep Dive (2026-03-24)

## Scope
- Logs analyzed: `logs/ethusdt.log`, `logs/solusdt.log`.
- Code paths analyzed:
  - `research/neural_alpha/runtime/shadow_session.py`
  - `research/regime/regime.py`
  - `core/execution/common/portfolio/portfolio_intent_engine.hpp`
  - `core/engine/trading_engine_main.cpp`

## Executive Summary

Your intuition is correct on both points:

1. **Regime behavior is unstable/messy during these runs**, especially because regime retraining cadence is tied to raw processed tick count, not wall-clock time. In high-throughput burst conditions, this retrains too frequently and causes fast regime-label churn.
2. **Alpha quality is poor in both runs** (negative IC / deeply negative ICIR, high gating rates).
3. **Neither run is long enough for promotion decisions** (14.5 min ETH, 31.4 min SOL vs explicit 2-week guidance in runtime report).

## What happened in ETHUSDT (`logs/ethusdt.log`)

### Session-level outcome
- Session ended after **14.5 minutes** with readiness warning still stating run should be >= 2 weeks.
- Alpha quality at end:
  - Realised IC: **-0.0121**
  - ICIR: **-15.1427**
  - Gated events: **1708 / 1931 (88.5%)**
  - Safe-mode events: **962 (49.8%)**
- Regime distribution average heavily concentrated in **illiquid** (`p_illiquid=0.912`).

### Why it stopped early
- ETH run terminates due **risk circuit breaker**, not natural window completion:
  - `Kill switch triggered reason=CIRCUIT_BREAKER`
  - `Global risk breach check=CROSS_VENUE_NETTING_CAP`
- This immediately shuts down feed handlers and engine.

### Regime + alpha behavior during run
- Regime retrain messages appear repeatedly with rapidly changing spread means/ranges.
- There are only **2 enter_long intents** and **1 flatten**, with most ticks in hold.
- Shock/illiquid gates periodically dominate decisioning and suppress exposure.

## What happened in SOLUSDT (`logs/solusdt.log`)

### Session-level outcome
- Session ran **31.4 minutes**, then was manually interrupted (`^C`) and runner exits with status 130.
- Alpha quality at end:
  - Realised IC: **-0.0259**
  - ICIR: **-10.9703**
  - Gated events: **443 / 454 (97.6%)**
  - Safe-mode events: **184 (40.5%)**
- Regime distribution average dominated by **shock** (`p_shock=0.643`).

### Structural feed-health issue driving behavior
- At startup, Binance snapshot is rejected:
  - `Snapshot rejected: majority of levels out of grid range ... skipped=5753 total=9742`
- After this, portfolio intent repeatedly includes `health_degraded` with only 2/3 healthy venues.
- Despite occasional positive alpha entries, signals are mostly gated/flattened under shock + health degradation.

## Code-level diagnosis

### 1) Retrain cadence is tick-count-driven and can become too frequent under bursty throughput

In `shadow_session.py`:
- `self._processed_ticks += len(ticks)` increments by **all fetched records each loop**.
- Continuous retrain and regime retrain trigger off thresholds in processed ticks (`continuous_train_every_ticks`, `regime_retrain_every_ticks`).

Implication:
- If bridge returns many queued ticks at once, thresholds are hit rapidly and retraining can happen every few seconds in wall-clock time.
- That drives fast model/cluster churn and unstable downstream intent reasons.

### 2) Regime semantic naming can be noisy because it is re-derived each retrain

In `regime.py`:
- `_semantic_regime_names` assigns labels by feature ranks each retrain.
- Combined with frequent retrains, this can create semantic drift where the practical meaning of `shock`/`illiquid` shifts too often.

### 3) Entry/flatten logic is very sensitive to regime + health gates

In `portfolio_intent_engine.hpp`:
- `shock_regime` at `p_shock >= 0.60` can force flatten / hold.
- `illiquid_regime` at `p_illiquid >= 0.55` can halve target.
- `health_degraded` scales target via `health_reduce_ratio` when venue health drops.

On SOL, this combines badly with startup venue health degradation, producing predominantly risk-off/hold outcomes.

### 4) Binance grid sizing is too tight for SOL snapshot depth in this run

In `trading_engine_main.cpp`:
- Grid level count is derived from `target_range_usd / tick_size`.
- Runtime log shows target range configured to **$40**, which was insufficient for Binance SOL snapshot depth at startup.

This appears to be the direct trigger for the SOL health-degraded regime.

## Evidence-based conclusions

1. **Regime algorithm appears “messy” in production behavior** largely due to retraining cadence coupling to processed tick volume (not elapsed time), not necessarily because HMM math is broken.
2. **Alpha quality is currently not deployable** for both ETH/SOL sessions (negative IC/ICIR + high gate rates).
3. **Run duration is insufficient** for any confidence decision, and both runs ended prematurely (risk halt for ETH, manual interrupt for SOL).

## High-impact fixes (prioritized)

1. **Decouple retrain triggers from raw tick count**
   - Add minimum wall-clock spacing (e.g., no retrain more than every 5–10 min).
   - Keep tick thresholds as secondary conditions, not sole trigger.

2. **Add regime stability controls**
   - Require minimum posterior confidence persistence before switching dominant regime.
   - Add hysteresis around `shock_flatten_threshold` / `illiquid_reduce_threshold` to avoid oscillatory hold/flatten behavior.

3. **Fix venue startup health for SOL-like books**
   - Increase `target_range_usd` for symbols with wider effective depth distribution.
   - Or allow a short dynamic-range bootstrap before strict fixed-grid enforcement.

4. **Treat runs < 2 weeks as diagnostics-only**
   - Keep this hard gate in promotion checklist.
   - Add explicit “insufficient duration” status in summary output to avoid accidental over-interpretation.

## Suggested follow-up instrumentation

- Emit retrain wall-clock deltas (`seconds_since_last_continuous_train`, `seconds_since_last_regime_train`) in logs.
- Emit regime label continuity metric (how often dominant regime changes per minute).
- Emit per-venue health timeline (healthy/unhealthy transitions with root cause tags).

