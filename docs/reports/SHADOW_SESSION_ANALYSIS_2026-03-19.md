# Shadow Session Analysis — BTC & SOL

**Date:** 2026-03-19
**Sessions analysed:** `models/shadow_BTC.txt`, `models/shadow_SOL.txt`
**Scope:** Feed layer reliability, data quality, model training stability, ensemble behaviour, and hard blockers preventing live promotion.

---

## Executive Summary

Both shadow runs expose the same five systemic faults. None of these are edge cases — they co-occur within the first minutes of every run and compound each other. The stack is not ready for live promotion in its current state. The issues below are ordered from most to least severe.

| Priority | Issue | Affects |
|----------|-------|---------|
| P0 | Negative base prices at snapshot time | SOL only |
| P1 | Coinbase feed fails to start in both runs | BTC + SOL |
| P1 | Kraken grid-range overflow — 961–968 levels skipped per snapshot | BTC |
| P2 | Repeated buffer-overflow re-snapshots during retraining | BTC + SOL |
| P2 | Ensemble safe mode and rollback instability | BTC + SOL |

---

## Issue 1 — Negative Base Prices (P0 Hard Blocker)

**Where it appears:** `shadow_SOL.txt` lines 17, 31, 42.

```
[INFO] Snapshot applied symbol=SOLUSDT sequence=28380075953 base_price=-410.5  bids=1000 asks=1000
[INFO] Snapshot applied symbol=SOLUSDT sequence=0            base_price=-410.51 bids=500  asks=500
[INFO] Snapshot applied symbol=SOLUSDT sequence=29614153064  base_price=-410.5  bids=400  asks=400
```

**What it means:**
A base price of −410.5 is a physical impossibility for any traded asset. This appears across all three feed handlers that successfully initialise for SOL (Binance, Kraken, OKX), which rules out a single-venue bug. The value −410.5 is suspiciously close to the negative of the real SOL price (roughly $145 scaled, or a sign-inverted tick-normalised representation). The most likely root cause is a sign error in the price-normalisation or grid-origin calculation inside the C++ snapshot handler — the book is being anchored to a negated or subtracted reference price rather than the actual mid.

**Impact:**
Every relative price feature derived from the book (bid/ask offsets from mid, depth-weighted mid, etc.) will be sign-inverted and garbage throughout the entire SOL session. The neural model trains on contaminated features from tick 0. Regime features such as `log_ret_1` and `queue_imbalance` will also be corrupted. The SOL realised IC of 0.0135 and 65 safe-mode events are both direct downstream consequences.

**What to fix:**
Audit the snapshot handler's `base_price` computation. Verify the sign convention in price-delta decoding; the Binance diff-stream encodes deltas as signed integers and the reconstruction must not negate the reference level. Add a hard assertion: if `base_price <= 0` after snapshot, reject the snapshot and trigger an immediate re-snapshot with an `ERROR`-level event rather than silently continuing.

---

## Issue 2 — Coinbase Feed Fails to Start (P1)

**Where it appears:**
- `shadow_BTC.txt` line 58: `[WARN] Coinbase feed failed to start symbol=BTCUSDT` — 30 seconds after the WebSocket was established (line 57).
- `shadow_SOL.txt` line 92: `[WARN] Coinbase feed failed to start symbol=SOLUSDT` — also ~30 seconds post-establishment.

```
# BTC — 08:03:43 WebSocket established, 08:04:13 failed to start (+30 s)
# SOL — 08:18:27 WebSocket established, 08:18:57 failed to start (+30 s)
```

**What it means:**
The 30-second gap is consistent in both runs. The WebSocket connects successfully but the feed handler never emits a snapshot-ready event within the timeout window, which triggers the failure. This is most likely a subscription acknowledgement or snapshot-fetch timeout: the Coinbase Advanced Trade API requires a subscription confirmation message before the order-book snapshot can be requested, and that confirmation may be timing out or arriving malformed. A 503 from Kraken (BTC line 32) in the same initialisation window suggests the environment had marginal connectivity at session start, but Coinbase fails deterministically in both runs while Kraken recovers, indicating a Coinbase-specific protocol or authentication issue rather than general network instability.

**Impact:**
Coinbase is consistently absent from the live book. The system reports four venues in its config (`BINANCE,KRAKEN,OKX,COINBASE`) but operates on three. Venue diversity is reduced by 25 % and cross-venue spread and depth signals are computed without the second-largest USD-spot venue. This biases depth features and potentially the regime model's `spread_20` feature.

**What to fix:**
- Log the exact failure reason (timeout waiting for subscription ack, snapshot fetch error, or parse failure) rather than emitting a generic `WARN` and continuing.
- Implement retry with backoff (2 s, 4 s, 8 s) before giving up, since this is a cold-start race condition.
- If Coinbase fails after retries, emit an `OPS_EVENT` so operators can act, and exclude Coinbase's signals from the ensemble rather than silently degrading depth features.
- Investigate whether the Coinbase feed handler's subscription message is being sent before the WebSocket handshake fully completes.

---

## Issue 3 — Kraken Grid-Range Overflow (P1)

**Where it appears:** `shadow_BTC.txt` lines 35 and 71.

```
[WARN] Snapshot levels out of grid range symbol=BTCUSDT skipped=968
[WARN] Snapshot levels out of grid range symbol=BTCUSDT skipped=961
```

**What it means:**
The Kraken snapshot contains ~1,000 price levels but the local order-book grid can only accommodate a narrow band around `base_price`. Approximately 97 % of Kraken's delivered book is being discarded at each snapshot. With only ~32–39 levels retained (500 reported minus 961–968 skipped), the Kraken book is effectively a stub.

For BTC at ~$69,700, a typical Kraken L2 snapshot spans several thousand basis points of range. If the local grid is calibrated for a tick size or normalised price step designed around a much smaller asset price (e.g. one sized for SOL at ~$145), it will be far too narrow for BTC and reject most levels. This is consistent with the fact that the same warning does not appear for SOL.

**Impact:**
`depth_20` and related cross-venue depth features for BTC are computed from a near-empty Kraken book. This distorts multi-venue imbalance, effective spread, and any feature that aggregates across venues. The BTC regime model's `depth_20` mean values will reflect Binance/OKX depth almost exclusively, masking true market microstructure.

**What to fix:**
- The grid range (in price ticks or normalised units) must be parameterised per symbol and scaled to the asset's nominal price. For BTC at $69 k, the grid must span at least ±2 % from mid (≈±$1,400) at a reasonable tick resolution.
- Add a validation step at snapshot time: if `skipped / total > 0.5`, emit an `ERROR` and consider the venue book invalid for that snapshot cycle rather than accepting a stub.
- Log the grid bounds at startup so this misconfiguration is immediately visible without needing to reproduce a live run.

---

## Issue 4 — Repeated Buffer-Overflow Re-Snapshots During Retraining (P2)

**Where it appears:** All continuous training cycles in both sessions.

**BTC** — 6 buffer-overflow events during 4 retraining cycles:
Lines 98, 137, 176, 233, 254, 312.

**SOL** — 7 buffer-overflow events during 5 retraining cycles:
Lines 24 (sequence gap, not overflow), 97, 124, 175, 205, 261, 280, 340.

Representative timing (BTC):
```
08:05:21 [WARN] Triggering re-snapshot reason=Buffer overflow   ← during fold 2 pretrain
08:07:01 [WARN] Triggering re-snapshot reason=Buffer overflow   ← during secondary training
08:08:41 [WARN] Triggering re-snapshot reason=Buffer overflow   ← start of next retrain
08:10:21 [WARN] Triggering re-snapshot reason=Buffer overflow
08:12:01 [WARN] Triggering re-snapshot reason=Buffer overflow
08:13:41 [WARN] Triggering re-snapshot reason=Buffer overflow
```

**What it means:**
The delta buffer fills up and overflows while the training process holds the CPU. The C++ feed handler continues to receive deltas from the exchange WebSocket but the consumer thread (or ring buffer reader) is blocked by the Python training loop. When training finishes and the consumer catches up, the buffer has overflowed and the local book is stale, triggering a full re-snapshot. This is a CPU contention / thread-priority problem: Python training and C++ delta processing share CPU on the same machine without isolation.

This is particularly damaging because re-snapshots inject latency spikes into the tick stream, creating discontinuities that the model then trains on. Buffer overflows during pretraining (e.g. BTC line 83 shows constant pretrain loss = 4.1079 across all 5 epochs — zero learning) suggest the book reconstruction is already corrupt before training even begins for those folds.

**Fingerprint in BTC training:**
BTC pretrain losses are identically `4.1079` across all 5 epochs for multiple folds (lines 81–85, 158–162, 234–238, 249–253, 309–314, 369–373). A constant pretrain loss means the spatial encoder is receiving near-zero-variance input — the book is flat/stale after buffer overflow, consistent with the "flat training data (5/5 features near-zero variance)" warning at line 221 and its recurrence at lines 297, 397.

**Impact:**
- Training data collected during buffer-overflow windows contains stale or discontinuous book states.
- The regime model collapses to 100 % calm for BTC in 3 out of 4 cycles (lines 222, 298, 398), because all five features are near-zero variance — a direct consequence of the stale feed.
- The challenger model is rejected every single time for BTC (lines 110, 185, 261, 361), which means the incumbent model is never updated during the session — continuous retraining provides no benefit.

**What to fix:**
- Pin the C++ feed handler threads to dedicated CPU cores, isolated from the cores running Python training (use `taskset` or `cpuset` cgroups).
- Increase the delta ring buffer size to absorb at least 60 seconds of market data at peak throughput.
- When a re-snapshot is triggered during an active training run, record a flag on the affected tick window so that training data collected during that window can be excluded or down-weighted.
- Consider pausing delta ingestion to a secondary overflow buffer during training rather than dropping events.

---

## Issue 5 — Ensemble Safe Mode and Rollback Instability (P2)

**Where it appears:**

**SOL** `shadow_SOL.txt` lines 94–96:
```
[shadow] ticks=61  signal_mean=200.57bps  signal_std=81.02bps  IC=0.089
[SAFE_MODE] no rollback champion available; publishing neutral signal only
[OPS_EVENT] ensemble_canary_rollback: {'reason': 'ic_degradation ensemble_ic=0.0894 primary_ic=0.1682 margin=0.02'}
```

**Final statistics:**
| Metric | BTC | SOL |
|--------|-----|-----|
| signal_count | 130 | 241 |
| signal_mean_bps | −31.2 | +53.1 |
| signal_std_bps | 353.9 | 321.9 |
| max_abs_signal_bps | 1602.9 | 1602.9 |
| realised_ic | −0.0216 | +0.0135 |
| safe_mode_events | 25 | **65** |

**What it means:**

1. **No rollback champion on SOL at tick 61.** The canary rollback mechanism fires because `ensemble_ic=0.0894` is below `primary_ic=0.1682 - margin=0.02 = 0.1482`. It attempts to roll back to the champion model, but no champion exists yet (the session is only 61 ticks old and the first retraining cycle has not completed). The system correctly falls back to neutral signal, but the root cause is that the session started with no stable champion — this should be resolved by requiring an offline-validated champion before any shadow session begins.

2. **65 safe-mode events for SOL** (one in every ~3.7 ticks) indicate the signal is highly erratic. This is directly caused by training on corrupted book data (negative base prices + buffer overflows). The model's outputs are unstable because its inputs are unstable.

3. **BTC realised IC = −0.0216.** Negative IC means the signal is slightly anti-predictive on average. This is consistent with: (a) the regime model collapsing to 100 % calm (no regime-conditional signal scaling), (b) challenger models being rejected every cycle (the primary model never improves), and (c) signal mean of −31 bps throughout the session.

4. **Max absolute signal of 1,602.9 bps in both sessions** is excessive. A 16 % single-tick signal is a signal of model or normalisation pathology, not genuine alpha. This is a downstream symptom of the corrupted base price and flat-book inputs.

**What to fix:**
- Require a pre-validated offline champion model before starting any shadow session. The canary rollback has no champion to fall back to if the session starts cold.
- Add signal clip / sanity bounds: reject any signal exceeding e.g. 200 bps and emit an `OPS_EVENT` rather than passing it to the ensemble. This prevents corrupted-book spikes from propagating.
- The ensemble IC degradation threshold should be symmetric: trigger rollback only if ensemble underperforms primary by `margin` *sustained over N ticks*, not on a single-tick measurement, to reduce false-positive rollbacks.
- Fix the upstream data issues (Issues 1–4) first — many safe-mode events will disappear once the book is clean.

---

## Model Metadata — Additional Observations

### BTC (`neural_alpha_btcusdt_latest.json`)
```json
"loss_direction": 0.16926,
"oos_holdout_mse": { "incumbent": 2.45e-06, "challenger": 1.86e-05, "selected": "incumbent" }
```
The direction loss of 0.169 is high (random guessing for a binary direction loss is ~0.693 with cross-entropy, but this is a regression direction loss, so 0.169 is not obviously bad in isolation). However, the challenger MSE is 7.6× worse than the incumbent across all four retrain cycles. This is not normal variance — it signals that each retrain cycle is producing a materially degraded model, consistent with flat/stale training data from buffer overflow. The incumbent model is the pre-session offline checkpoint and is never improved in-session for BTC.

### SOL (`neural_alpha_solusdt_latest.json`)
```json
"loss_direction": 0.18289,
"oos_holdout_mse": { "incumbent": 4.87e-05, "challenger": 6.31e-06, "selected": "challenger" }
```
The challenger was accepted for SOL (incumbent MSE 7.7× worse than challenger). This means the in-session retrain did improve the model for SOL — but this is against a very weak incumbent (4.87e-05 OOS MSE, vs BTC's 2.45e-06). The SOL incumbent started in poor shape, likely because it was trained on the same corrupted negative-base-price data.

### BTC Regime Model (`r2_regime_model_btcusdt.json`)
All four regime means are **identical** (all features near 0 except `queue_imbalance = −0.946`), and all variances are clamped at `1e-06` — the HMM floor. Transition matrix is uniform (symmetric 0.9 / 0.0333). This model has not learned anything: it is the fallback "anchored to calm" state that the regime trainer emits when it detects flat data (log line 221, 297, 397). It will assign every tick to a single regime with near-certainty and cannot provide useful regime conditioning.

### SOL Regime Model (`r2_regime_model_solusdt.json`)
By contrast, SOL's regime model has learned meaningful structure: differentiated means across regimes, realistic transition probabilities (0.952–0.966 self-stay), proper feature scales, and shock regime with high volatility (`vol_20=2.88`, `log_ret_1=0.056`). The model is technically plausible. However, it was trained on data with corrupted base prices, so the absolute scale of the features may not reflect true market conditions.

---

## Remediation Priority Order

1. **Fix negative base prices** — single-line sign audit in the C++ snapshot handler. Block the SOL session from running until resolved. This is the most impactful fix.

2. **Diagnose and fix the Coinbase feed timeout** — add retry logic and proper error logging. Relatively self-contained.

3. **Fix BTC grid range** — parameterise grid bounds per symbol by nominal price. Straightforward configuration change.

4. **Isolate training CPU from feed handler CPU** — requires deployment-level change (core pinning or process separation). Medium effort, high reward: fixes buffer overflows, stale books, and collapsed regime models simultaneously.

5. **Require offline champion before shadow session start** — process/operational change. Add a pre-flight check in `run_shadow.sh` that verifies a validated champion checkpoint exists before launching.

6. **Add signal clip bounds** — defensive measure in the ensemble output layer, low effort.

---

*Report generated from static analysis of `models/shadow_BTC.txt`, `models/shadow_SOL.txt`, `models/neural_alpha_btcusdt_latest.json`, `models/neural_alpha_solusdt_latest.json`, `models/r2_regime_model_btcusdt.json`, `models/r2_regime_model_solusdt.json`.*
