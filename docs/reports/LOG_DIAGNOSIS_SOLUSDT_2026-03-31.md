# SOLUSDT incident diagnosis (from available logs)

Date: 2026-03-31

## Scope clarification

`logs/solusdt.log` is **not present** in this repository snapshot.
Available runtime artifacts for this session are:

- `logs/btcusdt.log`
- `logs/neural_alpha_shadow.jsonl`

The findings below are therefore based on those two files.

## Direct validation of your symptoms

### 1) "Handlers are unhealthy (OKX and KRAKEN)"
Confirmed. Repeated `ShadowSummary` entries report:

- `KRAKEN.health_state = degraded`
- `OKX.health_state = degraded`
- root cause includes `snapshot_rejection: 1` for both venues

At the same time, `BINANCE` remains healthy. Effective healthy venue count in portfolio intents stays at `healthy_venues=1 enabled_venues=1`.

### 2) "Missing order counts data"
Confirmed. The log explicitly reports:

- `Order-count columns are present but contain only zeros`
- `continuous retrain skipped — all-zero order-count window bid_non_zero=0 ask_non_zero=0`

This happens at startup and again later during retrain.

### 3) "Mean signals is really low"
Confirmed by event-level signal stream (`logs/neural_alpha_shadow.jsonl`):

- events: **1206**
- mean signal: **-0.03050**
- mean absolute signal: **0.06770**

So raw model outputs are small and centered near zero.

### 4) "alpha is super good but pnl is 0"
Partly true but the mechanism is visible:

- Alpha diagnostics (`ic`, `icir`) in `ShadowSummary` are strong.
- Session accounting remains flat throughout: `realized_pnl=0`, `unrealized_pnl=0`, `net_pnl=0`.

This is consistent with a system that generates forecasts but does not pass execution preconditions.

### 5) "loss_risk is sky high"
Not directly verifiable from the two available files: `loss_risk` is not logged in `btcusdt.log` or `neural_alpha_shadow.jsonl`.

However, the previously produced report in this repo (`docs/reports/LOG_ANALYSIS_SOLUSDT_2026-03-31.md`) indicates high risk-loss contribution can occur from sparse-positive weighting and may dominate selection score. This is plausible but cannot be re-confirmed from the two runtime files alone.

### 6) "0% gated event and safe events but 0 pnl, system doesn't trade"
Confirmed:

- event stream has `gated=true` count **0/1206** and `safe_mode=true` count **0/1206**
- yet portfolio intent lines repeatedly show `intent=hold` with `signal_bps=0`
- no change in equity/PnL over all accounting snapshots

So yes: **the system is producing predictions but remains in HOLD and does not execute trades**.

## Why it is not trading (most likely chain)

1. **Venue availability collapse**
   - You started with `OKX,KRAKEN,BINANCE`, but only Binance is healthy; two venues are degraded.
   - This reduces execution confidence and cross-venue support.

2. **Negative net edge after cost model**
   - Portfolio intents repeatedly show `alpha_edge_bps=-8.8` with `expected_cost_bps=8.8`.
   - Decision keeps `signal_bps=0` and `intent=hold`.

3. **Regime lock / retrain suppression**
   - Continuous retrain is frequently skipped due to `illiquid_concentration_high` (52 skips in this window).
   - Retrain also skipped due to all-zero order-count features.

4. **Feature quality issue in count channels**
   - All-zero order-count columns remove depth-count signal and reduce feature richness.

Combined effect: good directional diagnostics (`ic`/`icir`) do not convert into tradable, cost-adjusted edge under current execution/risk constraints.

## Immediate fixes to test next run

1. **Recover OKX/KRAKEN snapshot path first**
   - Treat `snapshot_rejection` as blocker before judging alpha-to-PnL conversion.

2. **Fail fast on all-zero count windows**
   - Hard-fail or auto-disable count channels when `bid_non_zero=0 and ask_non_zero=0`, instead of silent degraded training.

3. **Log decision decomposition per tick**
   - Add explicit `raw_signal_bps`, `cost_bps`, `risk_penalty_bps`, `final_signal_bps`, `hold_reason`.
   - Right now we infer this from `alpha_edge_bps` and intent lines.

4. **Lower retrain starvation in illiquid regimes**
   - If skips dominate, either expand window or adjust concentration cap with guardrails.

5. **Expose loss components in runtime logs**
   - Include `loss_return`, `loss_direction`, `loss_risk`, and selection score to directly validate the "loss_risk sky high" claim.
