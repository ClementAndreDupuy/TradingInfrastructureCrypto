# Regime Instability Stabilization Plan (2026-03-24)

## Goal
Reduce regime-driven strategy churn so shadow/live intent decisions are stable enough for reliable alpha evaluation.

## Problem statement (from ETHUSDT/SOLUSDT shadow sessions)

Observed failures to address:
1. Regime retraining runs too frequently under bursty throughput because retrain triggers are tick-count based (`_processed_ticks`) rather than wall-clock bounded.
2. Regime semantic labels can drift between retrains, so identical market states can map to different semantic names over time.
3. Hard regime thresholds (`shock`/`illiquid`) can flip risk posture too quickly with insufficient hysteresis.
4. Venue startup health failures (e.g., snapshot rejection) amplify regime/risk-off behavior and mask alpha quality.

## Scope
Primary code paths:
- `research/neural_alpha/runtime/shadow_session.py`
- `research/regime/regime.py`
- `core/execution/common/portfolio/portfolio_intent_engine.hpp`
- `core/engine/trading_engine_main.cpp`

## Execution plan

### Phase 0 — Instrumentation + Baseline Lock (same day)

**Changes**
- Add wall-clock telemetry for retrains:
  - `seconds_since_last_continuous_train`
  - `seconds_since_last_regime_train`
- Add regime churn telemetry:
  - dominant regime change count
  - switches per minute
  - average posterior confidence of dominant regime
- Add per-venue health transition counters with root causes (snapshot rejection, resnapshot loop, bridge gaps).

**Acceptance criteria**
- Logs contain all new fields every report interval.
- A single script can compute retrain intervals and regime switch rate from one session log.

---

### Phase 1 — Retrain Cadence Guardrails (P0)

**Changes**
- Keep tick-based thresholds, but add hard wall-clock minimum spacing:
  - `min_continuous_train_interval_s` (default: 600)
  - `min_regime_train_interval_s` (default: 900)
- Skip retrain if minimum wall-clock interval has not elapsed, even if tick threshold is reached.
- Add startup warm-up window (e.g., first 5 minutes) where regime retrain is disabled.

**Acceptance criteria**
- During high-throughput sessions, retrains cannot occur more often than configured minimum spacing.
- In a 30-minute shadow run, expected regime retrain count is bounded and predictable.

---

### Phase 2 — Regime Semantic Continuity (P0)

**Changes**
- Persist semantic mapping anchor from previous artifact and align new clusters to prior means before naming.
- Add continuity penalty in cluster-to-name assignment (prefer stable mapping when distances are close).
- Emit `semantic_remap_events` metric whenever a label assignment changes from previous artifact.

**Acceptance criteria**
- For stable synthetic data, semantic remap events remain near zero.
- For live/shadow runs, remap frequency drops materially versus baseline.

---

### Phase 3 — Decision Hysteresis in Hot Path (P0)

**Changes**
- Introduce regime hysteresis in portfolio intent:
  - Enter-shock threshold > exit-shock threshold
  - Enter-illiquid threshold > exit-illiquid threshold
- Add minimum persistence (N consecutive ticks) before switching into `shock_regime` flatten logic.
- Keep emergency overrides for true risk-off and hard risk breaches.

**Acceptance criteria**
- Reduce rapid hold/flatten oscillations in shadow replay.
- Maintain risk controls during genuine stress events.

---

### Phase 4 — Venue Startup Health Hardening (P1)

**Changes**
- Add dynamic startup grid bootstrap when snapshot coverage exceeds fixed range.
- If fixed range is insufficient, temporarily expand range and emit explicit metric/event.
- Revert to configured fixed range after stable convergence window.

**Acceptance criteria**
- No immediate `majority of levels out of grid range` for targeted symbols (ETH/SOL) in shadow startup tests.
- Healthy venue ratio remains stable after startup.

---

### Phase 5 — Validation Gate Before Promotion (P0)

**Changes**
- Add explicit session quality gate requiring minimum observation window (multi-day shadow) and stability constraints:
  - regime switches/min cap
  - retrain cadence cap
  - safe-mode and gated-event ceilings
  - IC/ICIR floor

**Acceptance criteria**
- Session report outputs pass/fail for each gate.
- Promotion blocked unless all gates pass.

## Suggested defaults for first rollout
- `min_continuous_train_interval_s = 600`
- `min_regime_train_interval_s = 900`
- `regime_startup_warmup_s = 300`
- `shock_enter = 0.70`, `shock_exit = 0.50`
- `illiquid_enter = 0.65`, `illiquid_exit = 0.45`
- `regime_persistence_ticks = 5`

## Risks and mitigations
- **Risk:** Slower adaptation to genuine regime shifts.
  - **Mitigation:** keep emergency risk-off path independent of hysteresis.
- **Risk:** Added complexity across research/hot-path boundary.
  - **Mitigation:** phase rollouts; add replay tests per phase before enabling in shadow/live.

## Deliverable sequence
1. Phase 0 + Phase 1 in one PR (instrumentation + cadence guardrails).
2. Phase 2 in a second PR (semantic continuity).
3. Phase 3 in a third PR (hot-path hysteresis).
4. Phase 4 in a fourth PR (startup health bootstrap).
5. Phase 5 in a final PR (promotion gates and report integration).
