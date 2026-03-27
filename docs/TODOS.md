# Project TODOs

P0 — Critical (from 2026-03-26 shadow run)

1) Fix degenerate HMM regime retrain (spread collapse to tick size)

Why: 2nd regime retrain produced spread_means=[0.01, 0.01, 0.01, 0.01] / spread_range=0.0 — all clusters collapsed to the minimum tick. Retrain was correctly rejected but root cause is unresolved. The accepted 1st-retrain model also had 3/4 regimes with identical spread_means=4.953106, showing partial degeneracy was already present.
Tasks:
 Add spread-diversity regularisation or cluster-separation constraint to HMM fitting.
 Detect and log near-degenerate solutions (spread variance < threshold) before accepting.
 Add unit test: feeding flat spread data must not produce accepted degenerate model.


2) Investigate and fix anti-predictive direction head (loss_direction > 1.0)

Why: neural_alpha_solusdt_latest.json shows loss_direction=1.2459; random binary CE ≈ 0.693. Values above ~0.85 indicate the model is inverting signal. Realised IC was −0.013 throughout the session. Secondary model is worse at 1.261.
Tasks:
 Audit direction label construction — check for off-by-one or sign flip in target.
 Verify loss weight w_direction scaling does not produce gradient saturation.
 Add assertion: accepted model must have loss_direction < 0.80.


3) Fix HPO score explosion between retrains (~40× increase)

Why: Primary HPO best score jumped from 2.38 → 95.46 between the 1st and 2nd retrain; secondary went from 36.15 → 183.91. Directly caused by unstable regime assignments polluting the training window after the 1st (partially degenerate) retrain.
Tasks:
 Gate continuous retrain on ts_quality != "warn" or cap regime switches_per_minute before proceeding.
 Add HPO score sanity check: abort retrain and keep incumbent if best score exceeds N× previous score.
P2 — Minor (from 2026-03-26 shadow run)

4) Widen OrderBook grid to cover full snapshot depth

Why: At session start skipped=4352 total=9292 — 47% of Binance snapshot levels fell outside the $50 grid. SOL was trading near the grid top ($112 vs top=$111.71). Kraken/Coinbase also use tick_size=1 giving only 50 levels for the same $50 range.
Tasks:
 Set grid range dynamically from snapshot mid ± configurable factor (e.g. ±10%).
 Ensure Kraken/Coinbase level count is proportional to their tick size.