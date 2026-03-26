# SOLUSDT shadow run analysis (2026-03-26)

## Scope
- Source log: `logs/solusdt.log`
- Source model artifacts: `models/neural_alpha_solusdt_latest.json`, `models/neural_alpha_solusdt_secondary.json`, `models/r2_regime_model_solusdt.json`, `models/r2_regime_model_solusdt.json.meta.json`
- `logs/neural_alpha_shadow.jsonl` was empty during this run.

## Executive diagnosis
1. **Primary blocker: continuous retraining is failing repeatedly** due to a runtime type mismatch (`'<' not supported between instances of 'float' and 'str'`).
2. **Signal quality is negative and worsening** through the run (`ic` and `mean_effective_bps` stay below zero).
3. **Gating is too aggressive relative to current model quality** (`safe_mode_gate`, `confidence_gate`, and `horizon_gate` counts all accumulate quickly).
4. **Venue health stayed nominal**, but adaptive venue quality never warmed up (`sample_count=0` in periodic venue-quality logs), so router adaptation never meaningfully activated.

## Quantitative snapshot from the run
- Log lines: 626
- Shadow summary snapshots: 31
- Continuous retrain started: 70
- Continuous retrain failed: 69
- Snapshot out-of-grid warnings: 2
- Missing credential warnings: 6

### Summary drift (first vs final ShadowSummary)
- `ticks`: 97 → 601
- `ic`: -0.145523 → -0.107964
- `icir`: -6.212187 → -37.847170
- `mean_effective_bps`: -0.429741 → -2.770022
- `mean_raw_bps`: -124.255036 → -80.938809
- `safe_mode_gate`: 30 → 435
- `confidence_gate`: 89 → 527
- `horizon_gate`: 2 → 43
- `switches_per_minute`: 5.004172 → 1.383411
- `average_dominant_confidence`: 0.637400 → 0.729185

### HPO instability (trial-1 scores only, from log stream)
- Samples: 69
- min=0.425553, median=1.135788, max=2062.925094
- count(score > 10)=10, count(score > 100)=6

## Artifact readout
### Primary model (`models/neural_alpha_solusdt_latest.json`)
- `oos_holdout_mse`: incumbent=0.000597, challenger=0.000417, selected=challenger
- Loss stack still dominated by direction/risk terms (`loss_direction`≈0.922, `loss_risk`≈0.665)

### Secondary model (`models/neural_alpha_solusdt_secondary.json`)
- Higher total loss (≈1.451) vs primary selected fold snapshot.

### Regime model (`models/r2_regime_model_solusdt.json`)
- 4 regimes persisted (`calm`, `illiquid`, `trending`, `shock`).
- Initial probability is near-fully concentrated in `trending`; can cause sticky startup classifications.
- Meta spread range=3.152 (non-degenerate), so regime training itself appears to have run.

## Root causes (ranked)
1. **Type coercion bug in HPO path** (critical): likely config scalar loaded as string and compared with float score.
2. **Training loop effectively stale**: because continuous retrain fails, model quality cannot adapt intrarun.
3. **Over-gating under poor signal calibration**: policy blocks execution repeatedly while alpha remains misaligned.
4. **Insufficient post-run observability**: empty `logs/neural_alpha_shadow.jsonl` removes tick-level forensic trail.

## High-impact improvements
### A) Immediate stability fix (implemented in this commit)
- Force numeric casting for pipeline config constants (`large_selection_score`, `n_levels`, `request_timeout_s`, `holdout_frac`) so HPO comparisons stay numeric.
- Add a unit test that simulates string-valued `large_selection_score` and verifies HPO selection still works.

### B) Next improvements for your next run
1. **Fail-fast guardrails for config schemas**
   - Validate YAML numeric fields at startup with explicit type checks + coercion and log normalized config.
2. **Make retrain failures actionable**
   - Emit full exception type + stack trace and include current cfg trial payload.
3. **Regime/startup calibration**
   - Apply temperature smoothing to initial regime priors to avoid near-delta startup allocations.
4. **Gate-policy adaptation**
   - If rolling IC is negative over a minimum horizon, reduce confidence/horizon gating strictness while cutting position size (learning mode) rather than hard-zeroing all signals.
5. **Venue-quality activation checks**
   - Add a warning when venue quality `sample_count` stays zero beyond warmup; this catches dead telemetry paths early.
6. **Shadow JSONL reliability**
   - Assert the shadow log sink is writable and non-empty after first signal; if empty, raise and rotate to fallback path.

## “Incredible” upgrade path (practical roadmap)
1. **Meta-controller on top of alpha**
   - Train a lightweight online bandit that chooses between (primary, secondary, no-trade) using recent realized edge and calibration error.
2. **Cross-venue disagreement alpha**
   - Add feature channels for microprice divergence and queue-pressure spread across Binance/Kraken/OKX; use disagreement bursts as entry filter.
3. **Dynamic gating from expected shortfall budget**
   - Replace static threshold gating with ES-budget-aware gating: permit trades only when predicted edge minus cost exceeds adaptive risk budget.
4. **Continuous retrain sandbox mode**
   - Run candidate model in “ghost” mode for N ticks before promotion; promote only if online calibration improves vs incumbent.
