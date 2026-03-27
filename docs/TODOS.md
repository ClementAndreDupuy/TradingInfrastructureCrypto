# Project TODOs

Source report: `docs/SHADOW_RUN_SOLUSDT_2026-03-26.md`.

## P0 — Critical (must complete before next SOLUSDT shadow run)

### 1) Fix continuous retrain type-safety regressions in HPO path
- **Severity:** Critical
- **Why:** Shadow run showed `Continuous retrain failed: 69/70` with runtime type mismatch (`'<' not supported between instances of 'float' and 'str'`), which stalls model adaptation.
- **Scope:** `research/neural_alpha` training/HPO config loading + selection logic.
- **Tasks:**
  - [x] Add a single config-normalization layer that coerces numeric YAML/env scalars (`large_selection_score`, `n_levels`, `request_timeout_s`, `holdout_frac`, and similar fields) to validated numeric types.
  - [x] Enforce schema validation at startup with explicit per-field type errors and defaults.
  - [x] Ensure all score comparisons use numeric types only (no mixed string/float paths).
  - [x] Expand unit coverage for string-valued and malformed numeric inputs.
- **Acceptance criteria:**
  - [x] `python -m research.neural_alpha.pipeline --synthetic --ticks 400 --epochs 5` completes without type-comparison exceptions.
  - [x] New tests cover at least one string numeric input and one invalid non-numeric input; both assertions pass.
  - [x] In a 600+ tick SOLUSDT shadow run, retrain failure rate from type errors is exactly 0.

### 2) Make retrain failures fully diagnosable
- **Severity:** Critical
- **Why:** Current logs do not provide enough context to quickly isolate failing trial payloads.
- **Scope:** Exception handling + telemetry in continuous retrain loop.
- **Tasks:**
  - [x] Log exception class, message, and stack trace for each retrain failure.
  - [x] Log sanitized trial/config payload and selected model metadata for failing iteration.
  - [x] Add structured failure counters by error class in shadow summary output.
- **Acceptance criteria:**
  - [x] A forced retrain failure emits stack trace and trial context in a single searchable block.
  - [x] Shadow summary includes per-error-class failure counts.

## P1 — High (complete in next iteration)

### 4) Reduce over-gating under degraded signal quality
- **Severity:** High
- **Why:** Gate counters (`safe_mode_gate`, `confidence_gate`, `horizon_gate`) climbed rapidly while IC and effective bps remained negative.
- **Scope:** Gating policy + position sizing policy.
- **Tasks:**
  - [ ] Add rolling-IC-aware “learning mode”: relax confidence/horizon gates when IC stays negative over minimum horizon.
  - [ ] Couple relaxed gating with smaller notional/position caps to limit downside.
  - [ ] Add guardrails to prevent oscillation between strict and relaxed modes.
- **Acceptance criteria:**
  - [ ] In replay/shadow test with sustained negative IC, system enters learning mode deterministically.
  - [ ] Gate counts per 100 ticks decrease versus baseline while max drawdown remains within configured risk cap.

### 6) Improve startup regime calibration
- **Severity:** High
- **Why:** Initial regime probabilities were near-fully concentrated in `trending`, causing sticky startup behavior.
- **Scope:** Regime model initialization.
- **Tasks:**
  - [ ] Add temperature smoothing (or equivalent prior flattening) for startup regime probabilities.
  - [ ] Introduce configurable minimum entropy floor for initial distribution.
  - [ ] Add regression tests for startup prior concentration edge cases.
- **Acceptance criteria:**
  - [ ] Startup regime distribution entropy exceeds configured floor in deterministic test fixtures.
  - [ ] Regime assignment converges normally after warmup without persistent startup bias.

## P2 — Medium (roadmap enhancements after stabilization)

### 7) Add alpha meta-controller (primary / secondary / no-trade)
- **Severity:** Medium
- **Why:** Report recommends an online selector to mitigate miscalibrated alpha periods.
- **Scope:** Decision layer above existing models.
- **Tasks:**
  - [ ] Prototype contextual bandit selector using realized edge + calibration error.
  - [ ] Add shadow-only “no-trade” fallback arm with explicit selection logging.
  - [ ] Backtest selector vs static primary model baseline.
- **Acceptance criteria:**
  - [ ] Selector improves realized edge or reduces drawdown in shadow backtest over baseline.
  - [ ] Selection decisions are fully auditable per tick.

### 8) Add cross-venue disagreement feature set
- **Severity:** Medium
- **Why:** Cross-venue microprice/queue-pressure divergence may improve entry filtering.
- **Scope:** Feature engineering + training data pipeline.
- **Tasks:**
  - [ ] Add per-venue microprice divergence channels.
  - [ ] Add queue-pressure spread features across Binance/Kraken/OKX.
  - [ ] Evaluate disagreement burst filter on historical + shadow datasets.
- **Acceptance criteria:**
  - [ ] Feature pipeline produces stable values with no NaN/inf in standard runs.
  - [ ] Offline evaluation shows statistically significant precision gain for entry signals.

### 9) Replace static gating with ES-budget-aware gating
- **Severity:** Medium
- **Why:** Static thresholds are brittle when signal quality shifts.
- **Scope:** Risk/gating interface.
- **Tasks:**
  - [ ] Define expected-shortfall budget API exposed to gating logic.
  - [ ] Gate trades on net predicted edge (edge - cost - risk budget charge).
  - [ ] Add scenario tests for calm/shock/illiquid regimes.
- **Acceptance criteria:**
  - [ ] Trades are blocked when ES budget is exhausted even if raw confidence is high.
  - [ ] In calm regimes with available budget, gating allows eligible trades.

### 10) Continuous retrain ghost-mode promotion flow
- **Severity:** Medium
- **Why:** Safer online model promotion can prevent regressions.
- **Scope:** Model lifecycle in shadow/live.
- **Tasks:**
  - [ ] Run challenger model in ghost mode for configurable tick window.
  - [ ] Compare online calibration + realized edge vs incumbent during ghost window.
  - [ ] Promote only when challenger exceeds promotion thresholds; otherwise discard.
- **Acceptance criteria:**
  - [ ] Promotion decision is deterministic and logged with metric deltas.
  - [ ] Failed challengers never replace incumbent model.

---

*Items below sourced from shadow run `logs/solusdt.log` 2026-03-26.*

## P0 — Critical (from 2026-03-26 shadow run)

### 11) Fix degenerate HMM regime retrain (spread collapse to tick size)
- **Why:** 2nd regime retrain produced `spread_means=[0.01, 0.01, 0.01, 0.01]` / `spread_range=0.0` — all clusters collapsed to the minimum tick. Retrain was correctly rejected but root cause is unresolved. The accepted 1st-retrain model also had 3/4 regimes with identical `spread_means=4.953106`, showing partial degeneracy was already present.
- **Tasks:**
  - [ ] Add spread-diversity regularisation or cluster-separation constraint to HMM fitting.
  - [ ] Detect and log near-degenerate solutions (spread variance < threshold) before accepting.
  - [ ] Add unit test: feeding flat spread data must not produce accepted degenerate model.

### 12) Investigate and fix anti-predictive direction head (loss_direction > 1.0)
- **Why:** `neural_alpha_solusdt_latest.json` shows `loss_direction=1.2459`; random binary CE ≈ 0.693. Values above ~0.85 indicate the model is inverting signal. Realised IC was −0.013 throughout the session. Secondary model is worse at 1.261.
- **Tasks:**
  - [ ] Audit direction label construction — check for off-by-one or sign flip in target.
  - [ ] Verify loss weight `w_direction` scaling does not produce gradient saturation.
  - [ ] Add assertion: accepted model must have `loss_direction < 0.80`.

### 13) Fix HPO score explosion between retrains (~40× increase)
- **Why:** Primary HPO best score jumped from 2.38 → 95.46 between the 1st and 2nd retrain; secondary went from 36.15 → 183.91. Directly caused by unstable regime assignments polluting the training window after the 1st (partially degenerate) retrain.
- **Tasks:**
  - [ ] Gate continuous retrain on `ts_quality != "warn"` or cap regime `switches_per_minute` before proceeding.
  - [ ] Add HPO score sanity check: abort retrain and keep incumbent if best score exceeds N× previous score.

## P2 — Minor (from 2026-03-26 shadow run)

### 14) Widen OrderBook grid to cover full snapshot depth
- **Why:** At session start `skipped=4352 total=9292` — 47% of Binance snapshot levels fell outside the $50 grid. SOL was trading near the grid top ($112 vs top=$111.71). Kraken/Coinbase also use `tick_size=1` giving only 50 levels for the same $50 range.
- **Tasks:**
  - [ ] Set grid range dynamically from snapshot mid ± configurable factor (e.g. ±10%).
  - [ ] Ensure Kraken/Coinbase level count is proportional to their tick size.

