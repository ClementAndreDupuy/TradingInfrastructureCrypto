# Log Analysis — `logs/solusdt.log` (2026-03-31)

## Executive summary

The model is not learning from this run because the training signal quality collapses during continuous retraining. The largest contributors are:

1. **Feature collapse in order-count channels** (all-zero order count columns), which flattens a full block of count-aware LOB features.
2. **A regime/data-distribution shift to persistent illiquid state** right before the loss jump.
3. **Highly unstable multitask objective under sparse risk labels**, where BCE positive weighting can become very large and dominate the selection score.
4. **Model-selection gate rejecting anti-predictive models**, so retrains keep being discarded and behavior appears "not learning".

## Evidence from the log

- The file name suggests `solusdt`, but the recorded run is actually **BTCUSDT** from startup onward.
- Order-count columns are present but all-zero at both retrain cycles.
- Initial primary HPO scores are normal (~0.94 to ~1.29), then after continuous retrain they jump to ~57-77.
- Challenger model is rejected for high direction loss (anti-predictive).
- Secondary model is disabled by sanity checks.
- Right before the blow-up, regime transitions to fully illiquid (`p_illiquid=1`).

## Root-cause hypotheses mapped to code

### 1) Count-aware LOB features are effectively disabled

`pipeline.py` explicitly warns when order-count columns are all zeros. In this case, count features are present in schema but carry no information.

In feature construction, count channels are normalized and included in the LOB tensor. If inputs are all zero, these channels become structurally flat and reduce effective feature diversity.

**Impact:** weaker representation learning, poorer generalization, and more dependence on few noisy features.

### 2) Data distribution shift at continuous retrain window

The loss explosion appears immediately after `continuous retrain started ... window_ticks=1000` while regime is locked illiquid (`p_illiquid=1`). That suggests the 1000-tick window is dominated by a different microstructure regime than the earlier window.

**Impact:** tuned hyperparameters selected on prior distribution become poor for the new window; walk-forward score spikes.

### 3) Risk-task weighting can destabilize total selection score under sparse positives

Class weights are recomputed from current window. For risk BCE, `pos_weight = risk_neg / max(risk_pos, 1.0)`.

If risk positives are very sparse in a retrain window, `pos_weight` can become very large, amplifying BCE and making the aggregate selection score explode.

**Impact:** sudden multi-order-of-magnitude score increases without necessarily requiring a numerical bug.

### 4) "Doesn't learn" symptom is amplified by rejection guards

The system rejects challenger models when direction loss exceeds threshold (0.8). This is visible in the run (`direction loss 1.0362`). Secondary model is also disabled by sanity checks.

So retraining can happen, but deployed state may stay incumbent/rejected, which operationally looks like no learning.

## Why this is likely not a simple LR instability

HPO trials use conservative LR (`2e-4`) and gradient clipping exists (`clip_grad_norm_` with `grad_clip=1.0`). Explosion starts after regime/label-quality changes, not after LR changes.

## Immediate checks to confirm

1. Dump risk label prevalence (`adv_sel`) per retrain window and track resulting `risk_pos_weight`.
2. Log per-component validation losses (`loss_return`, `loss_direction`, `loss_risk`) beside HPO score.
3. Add a hard guard for all-zero order-count columns in shadow continuous training (warn-only today).
4. Segment HPO score by regime state; avoid training windows that are near-pure illiquid.
5. Verify this was intended symbol: log file name says SOL but run uses BTC.

## Most probable primary reason

**Training window quality collapse** (all-zero count features + illiquid-regime concentration + sparse risk labels inflating risk BCE weighting), leading to very high selection loss and model rejection.
