# Futures vs Spot Trading Path Review (Alpha Generation → Order Submission)

Date: 2026-03-29

## End-to-end flow (current implementation)

1. **Alpha generation (research cold path)**
   - `NeuralAlphaShadowSession` consumes LOB snapshots from the shared-memory ring, runs the primary/secondary model stack, applies confidence/risk/safe-mode gating, and publishes `(signal_bps, risk_score, size_fraction, horizon_ticks, ts_ns)` to mmap. Regime probabilities are published in parallel.  
   - Source: `research/neural_alpha/runtime/shadow_session.py`, `_infer`, `_apply_signal_gates`, publisher calls.

2. **Signal ingestion (C++ hot path)**
   - `AlphaSignalReader` and `RegimeSignalReader` read seqlock-framed mmap payloads and expose basic staleness + threshold checks.
   - Source: `core/ipc/alpha_signal.hpp`, `core/ipc/regime_signal.hpp`.

3. **Intent generation**
   - `PortfolioIntentEngine::evaluate` converts alpha/risk/regime + position state + venue quality into target position, urgency, flatten logic, and reason codes.
   - Source: `core/execution/common/portfolio/portfolio_intent_engine.hpp`.

4. **Routing and scheduling**
   - `SmartOrderRouter::route_with_alpha` blocks on alpha risk/min-signal gates and scales quantity by signal magnitude.
   - Source: `core/execution/router/smart_order_router.cpp`.

5. **Order submission**
   - In `trading_engine_main.cpp`, mode selection determines execution target:
     - `spot_only`: route to Binance/Kraken/OKX/Coinbase spot connectors.
     - `futures_only`: force Binance futures connector with futures-specific fields (`position_mode`, `position_side`, `reduce_only`).
   - Source: `core/engine/trading_engine_main.cpp`.

## Spot vs futures comparison

## 1) Alpha source
- **Spot path:** Alpha is generated from spot LOB ring data (`BINANCE`, `OKX`, `COINBASE`, `KRAKEN` exchange map in the bridge).
- **Futures path:** Uses the *same* alpha source in current architecture; startup logs explicitly record `alpha_source=spot` even when execution target is futures.

**Implication:** This is a spot-driven alpha with futures execution overlay, not a futures-native alpha stack.

## 2) Strategy mode and venue enablement
- `strategy_mode` is required (`spot_only` or `futures_only`).
- In `spot_only`, non-Binance spot venues can run.
- In `futures_only`, Binance futures connector must be enabled; spot execution connectors are effectively bypassed for submits.

## 3) Intent and sizing logic
- Same `PortfolioIntentEngine` is used in both modes.
- It computes basis slippage term from `spot_mid_price` and `futures_mid_price`; however, in current engine loop the futures mid is set from Binance spot book mid in futures mode.

**Implication:** Basis guard exists conceptually, but runtime input is not yet true spot-vs-perp basis in the main loop.

## 4) Risk stack
- **Shared controls:** circuit breaker, kill switch, generic global risk checks, affordability accounting.
- **Futures-specific capabilities exist in code:** leverage/funding/maintenance/mark-index checks (`check_futures_order` / `commit_futures_order`) and Binance futures pre-trade normalization/filters.
- **Current integration gap:** engine submit path currently calls generic `commit_order(...)` rather than `commit_futures_order(...)` in futures mode.

**Implication:** Futures risk gate logic is implemented but not wired into live submit decision path.

## 5) Connector semantics
- **Spot Binance connector:** supports market/limit/LIMIT_MAKER, rejects stop-limit usage.
- **Binance futures connector:** supports futures-specific validation and params (`positionSide`, `reduceOnly`, `STOP_MARKET` close-position semantics), plus symbol filter fetch/normalization.

## Key strengths
- Clear runtime mode separation with startup validation.
- Futures connector is materially richer than a spot clone (proper order model + exchange filters).
- Intent framework already includes regime/health/staleness and basis hooks for futures overlay behavior.

## Key gaps / risks
1. **Futures risk gate wiring gap**: futures submissions do not currently invoke `commit_futures_order(...)`.
2. **Basis signal quality gap**: futures mode populates `futures_mid_price` from spot book mid, weakening basis divergence protection.
3. **Config mismatch risk**: `config/live/engine.yaml` defaults to `spot_only` and keeps futures disabled, so futures path requires explicit operator changes and validation.

## Recommended next actions (ordered)
1. Wire futures-mode submit path to `GlobalRiskControls::commit_futures_order(...)` with a populated `FuturesRiskContext`.
2. Feed true Binance futures mark/contract mid into `PortfolioIntentContext.futures_mid_price` (or rename/remove basis guard until true futures mid is available).
3. Add integration tests covering:
   - futures funding-cost scaling,
   - mark/index divergence rejection,
   - long→flat and short→flat reduce-only correctness.
