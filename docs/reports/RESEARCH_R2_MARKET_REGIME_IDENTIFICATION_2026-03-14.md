# R2 Research — State-of-the-Art Real-Time Market Regime Identification

Date: 2026-03-14
Scope: Research + implementation blueprint for dynamic regime-aware strategy switching in ThamesRiverTrading.

## 1) Objective

Design a **production-grade, real-time market regime identification stack** that:

1. Detects regime shifts fast enough for crypto microstructure trading.
2. Produces stable, interpretable regime labels for risk + execution.
3. Integrates cleanly with this repository split:
   - **Python cold path** for model training/validation/research.
   - **C++ hot path** for low-latency inference/routing/risk gating.
4. Enables dynamic policy adaptation (quote width, skew, participation, venue selection, kill-switch sensitivity).

---

## 2) What “state of the art” means in 2024–2026 practice

In production trading systems, SOTA regime detection is **not one model**; it is an ensemble pipeline combining:

1. **Latent-state probabilistic models**
   - HMM/HSMM and Markov-switching VAR for interpretable latent states.
   - Strong baseline for robust, explainable regime probabilities.

2. **Online change-point detection (CPD)**
   - BOCPD / CUSUM / GLR / Page-Hinkley for fast break detection.
   - Critical to catch abrupt transitions (liquidity shocks, news, liquidations).

3. **Representation learning + clustering**
   - Sequence embeddings (Transformer/TCN/contrastive encoders) + clustering (HDBSCAN, spectral, k-means online variants).
   - Captures nonlinear structure missed by linear-state models.

4. **Hierarchical regimes**
   - Macro regime (trend/range/crisis) + micro regime (order-flow imbalance, spread regime, toxicity regime).
   - Better alignment with execution decisions at sub-second horizons.

5. **Meta-labeling / policy gating layer**
   - Regime probabilities are turned into action constraints (e.g., max quote size, inventory limit multiplier, taker aggression).
   - Avoids hard brittle “if regime==X then strategy Y only”.

**Conclusion:** best practice is a **hybrid ensemble** with calibrated probabilities, not a single clustering or HMM-only approach.

---

## 3) Regimes that matter for this codebase

Given current components (`orderbook`, `feeds`, `risk`, `execution`, `neural_alpha`), the most actionable regime taxonomy is:

## 3.1 Primary (macro-execution) regimes

1. **Calm mean-reverting**
   - Low realized volatility, stable spread/depth, low toxicity.
   - Favor tighter quoting, larger passive participation.

2. **Trending directional**
   - Persistent signed order-flow + drift.
   - Favor asymmetric skew, tighter risk limits on adverse side.

3. **Volatile/liquidation shock**
   - High short-horizon volatility, spread expansion, depth collapse, elevated cancel rates.
   - Widen quotes, reduce size, stricter circuit breaker thresholds.

4. **Illiquid/stale-feed risk**
   - Missing updates, sequence gaps, high latency/staleness, fragmented book.
   - Throttle/disable quoting, prioritize safety.

## 3.2 Secondary (microstructure) overlays

- **Spread state:** tight / normal / wide.
- **Depth state:** thick / thin.
- **Toxic flow state:** low / medium / high adverse-selection risk.
- **Cross-venue dislocation state:** aligned / diverged.

This hierarchy maps directly to existing risk and execution controls in C++.

---

## 4) Candidate methods: strengths, weaknesses, suitability

## 4.1 HMM / Markov-switching models

**Pros**
- Interpretable latent states with transition matrix.
- Cheap inference (forward filter) suitable for near-real-time.
- Probabilistic outputs ideal for risk gating.

**Cons**
- Standard HMM assumes geometric state durations (often unrealistic).
- Can lag at abrupt changes without CPD assistance.

**Fit**
- Excellent baseline + production core, especially with Student-t emissions or robust covariance.

## 4.2 HSMM (Hidden semi-Markov)

**Pros**
- Explicit duration modeling; avoids over-frequent state flipping.
- More realistic for market regimes with persistence.

**Cons**
- Slightly heavier computation/implementation complexity.

**Fit**
- Strong upgrade once HMM baseline is stable.

## 4.3 Online change-point detection (BOCPD/CUSUM)

**Pros**
- Very fast detection of structural breaks.
- Complements HMM lag.

**Cons**
- Can over-trigger without robust thresholds/features.

**Fit**
- Mandatory guardrail for crypto shock environments.

## 4.4 Clustering on handcrafted features (k-means/GMM/HDBSCAN)

**Pros**
- Simple implementation, good exploratory discovery.
- HDBSCAN handles variable density and outliers better.

**Cons**
- Offline labels unstable across retrains unless anchored.
- Weak temporal consistency without smoothing.

**Fit**
- Good for discovery and regime dictionary initialization.

## 4.5 Deep sequence regime encoders (Transformer/TCN + clustering/classifier)

**Pros**
- Captures nonlinear temporal patterns and cross-feature interactions.
- Can ingest richer order-book/flow context.

**Cons**
- More ops complexity, drift risk, explainability challenges.

**Fit**
- Use as secondary signal in cold path first; promote only with strict monitoring.

---

## 5) Recommended architecture (best-practice for this repo)

## 5.1 Three-layer regime engine

1. **Layer A — Fast break detector (CPD):**
   - Inputs: short-horizon volatility burst, spread jump, depth collapse, feed staleness.
   - Output: `shock_probability`, `break_flag`.

2. **Layer B — Probabilistic latent regime model (HMM/HSMM):**
   - Inputs: standardized feature vector at 100ms–1s bars.
   - Output: `P(regime=k)` over primary regimes.

3. **Layer C — Microstructure overlay classifier:**
   - Lightweight classifier/rules for spread/depth/toxicity states.
   - Output: overlay tags + confidence.

Final regime state = calibrated fusion of A+B+C with hysteresis.

## 5.2 Why this is optimal here

- Uses Python research strengths for training/evaluation.
- Keeps C++ hot path simple: consume compact probabilities/tags from shared memory.
- Robust to both gradual and abrupt changes.
- Supports explainable controls for risk/compliance review.

---

## 6) Feature set (real-time feasible, high signal)

## 6.1 Core features per symbol (100ms–1s)

1. **Volatility**
   - Realized variance, bipower variation, downside semivol.
2. **Liquidity**
   - Best spread, relative spread, top-N depth, depth slope/convexity.
3. **Order flow**
   - OFI, queue imbalance, signed trade imbalance, cancel-to-trade ratio.
4. **Microstructure stress**
   - Book resiliency (replenishment half-life), gap frequency, sweep events.
5. **Execution quality proxies**
   - Short-horizon slippage estimate, fill-to-markout adverse selection.
6. **System/venue health**
   - Feed lag, sequence gaps, reconnect counts, per-venue stale indicators.

## 6.2 Cross-venue features

- Mid-price dispersion across exchanges.
- Lead-lag signals (who moves first).
- Fragmentation index (where displayed depth resides).

These are key for dynamic routing and venue throttling.

---

## 7) Labeling + evaluation protocol (critical)

## 7.1 Regime labeling approach

Use a two-stage process:

1. **Unsupervised discovery** on historical periods (HDBSCAN/GMM + temporal smoothing).
2. **Semantic mapping** to operational regimes using objective metrics:
   - vol quantiles,
   - spread/depth stats,
   - toxicity/markout profiles,
   - stability duration.

Then train a supervised/lightweight sequential classifier to reproduce labels online.

## 7.2 Validation methodology

- Walk-forward splits by time (no leakage).
- Cross-exchange robustness checks (Binance/Kraken/OKX/Coinbase).
- Stability metrics:
  - Adjusted Rand Index across retrains.
  - Regime persistence / flip-rate.
- Economic value metrics:
  - PnL delta vs non-regime baseline,
  - drawdown reduction,
  - tail-risk mitigation,
  - execution shortfall improvement.

## 7.3 Guard against false confidence

Require every regime model release to pass:

- minimum out-of-sample persistence,
- minimum strategy uplift net fees/slippage,
- stress-period performance (high-vol windows),
- calibration quality (Brier score / reliability curve for regime probabilities).

---

## 8) Integration plan for ThamesRiverTrading

## 8.1 New Python cold-path modules

Under `research/neural_alpha/` add:

1. `regime_features.py`
   - Build real-time-compatible feature pipeline (Polars).
2. `regime_models.py`
   - HMM/HSMM + CPD + calibration utilities.
3. `regime_train.py`
   - Walk-forward training, model selection, persistence.
4. `regime_eval.py`
   - Stability/economic evaluation report generator.
5. `regime_export.py`
   - Export compact inference artifact (transition/emission params, scaler, thresholds).

## 8.2 New C++ hot-path components

1. `core/ipc/regime_signal_reader.hpp`
   - Shared memory reader for regime probabilities + metadata.
2. `core/execution/regime_policy.hpp`
   - Deterministic mapping from regime vector to execution parameters.
3. `core/risk/regime_risk_overrides.hpp`
   - Regime-aware tightening/loosening of limits.
4. `core/common/types.hpp` update
   - Add `RegimeState` struct with timestamp, probs, confidence, stale flag.

## 8.3 Runtime data contract (IPC)

Publish from Python to shared memory every 100ms–500ms:

- `ts_ns`
- `symbol`
- `p_calm`, `p_trend`, `p_shock`, `p_illiquid`
- `overlay_flags` bitmask (wide_spread, thin_depth, toxic_flow, dislocation)
- `model_version`
- `is_stale`

C++ must fail safe:
- if stale/missing signal → revert to conservative default policy.

---

## 9) Execution/risk policy mapping (example)

For each symbol, compute policy multipliers from probabilities:

- `quote_width_mult = 1 + 1.2*p_shock + 0.6*p_illiquid`
- `size_mult = 1 - 0.7*p_shock - 0.5*p_illiquid`
- `inventory_limit_mult = 1 - 0.6*p_shock`
- `taker_aggression_mult = 1 + 0.8*p_trend`

Add hysteresis and minimum dwell times to prevent oscillation.

Risk overrides:
- If `p_shock > threshold_hi` OR `thin_depth + toxic_flow` true, tighten circuit breaker thresholds.
- If feed stale, hard block new passive quoting until healthy.

---

## 10) Implementation phases (pragmatic)

## Phase 0 — Baseline instrumentation (1 week)
- Ensure all required features are logged to Parquet with consistent timestamps.
- Add cross-venue synchronization checks.

## Phase 1 — Offline research baseline (1–2 weeks)
- Build HMM + CPD prototype on historical data.
- Produce first stable 4-regime taxonomy and evaluation report.

## Phase 2 — Shadow inference (1–2 weeks)
- Run real-time inference in Python during shadow sessions.
- Compare regime outputs vs realized outcomes and execution quality.

## Phase 3 — C++ policy integration (1 week)
- Integrate IPC reader + policy mapping in market maker/risk checks.
- Conservative defaults + stale-signal fallback.

## Phase 4 — Controlled promotion (2+ weeks)
- Canary by symbol/notional.
- Enforce rollback conditions on flip-rate, drawdown, or fill-quality degradation.

---

## 11) Concrete backlog for this repo

1. Add regime research modules and tests in `research/neural_alpha/`.
2. Add `RegimeState` IPC schema and reader in `core/ipc/`.
3. Add execution/risk policy adapters in `core/execution/` and `core/risk/`.
4. Add replay tests:
   - deterministic regime signal playback,
   - policy output invariants,
   - stale-signal fail-safe behavior.
5. Add monitoring dashboards:
   - regime probabilities,
   - flip rate,
   - policy multipliers,
   - PnL/shortfall by regime.

---

## 12) Risks and mitigations

1. **Regime overfitting**
   - Mitigation: strict walk-forward + stress-period holdouts + simpler baseline precedence.
2. **State-flip noise (churn)**
   - Mitigation: hysteresis, duration constraints, HSMM, probability smoothing.
3. **Operational complexity**
   - Mitigation: versioned artifacts, canary rollout, automatic rollback triggers.
4. **Signal staleness / IPC failure**
   - Mitigation: TTL checks and conservative default risk posture in C++.

---

## 13) Recommended decision

Adopt a **hybrid CPD + HMM/HSMM + microstructure overlay** architecture as the production target for R2.

This approach is currently the best balance of:
- predictive power,
- interpretability,
- low-latency deployability,
- operational safety for live crypto execution.

It is directly implementable within the existing ThamesRiverTrading architecture and provides a clear path to measurable execution/risk improvements.

---

## 14) Implementation update (2026-03-17)

Implemented in codebase with a practical first production candidate:

1. Added `research/neural_alpha/regime.py` with a lightweight real-time regime trainer:
   - Reads LOB snapshots from an IPC directory (`/ipc` by default).
   - Builds real-time features (`vol_20`, `spread_20`, `depth_20`, `queue_imbalance`, `log_ret_1`).
   - Trains a compact K-Means regime model constrained to **3 or 4** regimes.
   - Assigns semantic regime names (`calm`, `shock`, plus `illiquid` / `trending` where available).
   - Persists artifact JSON (`version`, feature columns, scales, regime centers, regime names).

2. Integrated into `research/neural_alpha/pipeline.py` so regime training runs alongside neural alpha training.
   - New CLI flags:
     - `--ipc-dir` (default: `/ipc`)
     - `--regimes` (supported: `3` or `4`)
     - `--save-regime-model` (default: `models/r2_regime_model.json`)
   - Pipeline now trains/saves the R2 regime model in the same run as neural model training.

3. Added unit coverage in `tests/unit/test_regime.py`:
   - Verifies IPC ingestion and artifact persistence.
   - Verifies invalid regime-count rejection.

This establishes R2 as a working cold-path component that is trainable, reproducible, and persisted for downstream deployment/integration.


4. Added runtime IPC publishing + hot-path consumption for regimes:
   - `shadow_session.py` now publishes regime probabilities to `/tmp/regime_signal.bin` each inference tick.
   - `core/ipc/regime_signal.hpp` added seqlock reader for `p_calm/p_trending/p_shock/p_illiquid`.
   - `core/execution/market_maker.hpp` now reads regime IPC before quoting and blocks/widens orders under shock/illiquid states.

