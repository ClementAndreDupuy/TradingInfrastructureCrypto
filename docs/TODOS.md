# Project TODOs

---

## CRITICAL

---

## HIGH

---

## MEDIUM

- [ ] **FUT-BN-8: Shadow-mode rollout plan for Binance futures**
  - Scope: shadow execution path for futures with no live order placement, plus metrics for slippage, reject rate, and reconciliation drift.
  - Acceptance criteria:
    - [ ] Shadow mode can run as a dedicated session with futures trading explicitly activated (`futures_enabled=true`) while final order-placement side effects remain disabled.
    - [ ] Shadow mode reuses production code paths except final submit side effects.
    - [ ] Daily report includes futures-specific health metrics and drift deltas.
    - [ ] Go-live checklist defines promotion criteria and rollback conditions.

- [ ] **FUT-BN-9: Observability and audit trail for futures lifecycle**
  - Scope: structured logs + metrics for futures request/response latencies, error buckets, and order lifecycle transitions.
  - Acceptance criteria:
    - [ ] Metrics include per-endpoint p50/p95/p99 latency and reject taxonomy.
    - [ ] Audit logs can correlate `client_order_id` to Binance futures order IDs and reconciliation events.
    - [ ] No secrets appear in logs.

---

## LOW

- [ ] **PORT-L1: `check_stop` in `NeuralAlphaMarketMaker` ignores its `AlphaSignal` parameter**
  - Source: `AUDIT_PORTFOLIO_LONG_SHORT_2026-03-31.md` / LOW-1
  - File: `core/execution/market_maker.hpp:193`
  - Problem: `void check_stop(double mid, const AlphaSignal& )` receives a signal but the parameter is unnamed and unused. Signal-informed stop widening (e.g., tighter stop when risk is elevated) is not implemented, and the unnamed parameter creates confusion about whether this was intentional.
  - Acceptance criteria:
    - [ ] Either name the parameter and document why it is intentionally unused (with a `(void)` suppress), or implement signal-aware stop adjustment as a separate TODO with a design note.
    - [ ] No functional behavior change in this ticket.

- [ ] **PORT-L2: `AlphaSignalReader::read()` does not auto-reconnect; `RegimeSignalReader::read()` does**
  - Source: `AUDIT_PORTFOLIO_LONG_SHORT_2026-03-31.md` / LOW-2
  - Files: `core/ipc/alpha_signal.hpp:67–69`, `core/ipc/regime_signal.hpp:62–65`
  - Problem: If `/tmp/trt_ipc/trt_alpha.bin` appears after engine startup, alpha signals are never picked up without a full restart. The regime reader auto-reconnects. This asymmetry is undocumented and surprising.
  - Acceptance criteria:
    - [ ] `AlphaSignalReader::read()` attempts `open()` if `ptr_ == nullptr` (mirroring regime reader behavior), OR the asymmetry is explicitly documented with a rationale comment in both headers.
    - [ ] Chosen behavior is consistent between both readers.

- [ ] **PORT-L3: `intent_action()` returns `"reduce"` for both reducing longs and reducing shorts**
  - Source: `AUDIT_PORTFOLIO_LONG_SHORT_2026-03-31.md` / LOW-3
  - File: `core/engine/trading_engine_main.cpp:224–232`
  - Problem: A negative `position_delta` maps to `"reduce"` whether the intent is closing a long, adding to a short, or reducing a short. Shadow-mode logs label a short-entry as `"reduce"`, breaking intent tracking in observability dashboards.
  - Acceptance criteria:
    - [ ] Action labels distinguish direction: `"reduce_long"` (selling from long) vs `"enter_short"` (initiating short from flat) vs `"reduce_short"` (buying back a short) vs `"enter_long"`.
    - [ ] Labels require `current_position` context to determine; `intent_action` signature is updated accordingly, or a new helper is introduced.
    - [ ] Shadow metrics and Grafana dashboards (if any) are updated to handle new label values.

- [ ] **FUT-BN-10: Multi-Assets Mode and portfolio-margin compatibility review**
  - Scope: document compatibility matrix for one-way vs hedge mode, single-asset vs multi-assets mode, and future portfolio-margin migration.
  - Acceptance criteria:
    - [ ] Supported combinations are explicitly documented with constraints.
    - [ ] Unsupported account modes fail with clear startup diagnostics.

---

## RESEARCH

- [ ] **NA-R-4: Gate continuous retrain on window-quality health before HPO**
  - Scope: prevent low-information windows from producing noisy retrains that are predictably rejected.
  - Acceptance criteria:
    - [ ] Continuous retrain is skipped when order-count non-zero coverage is below a configurable threshold.
    - [ ] Continuous retrain is skipped when risk positive prevalence is below a configurable threshold (or fallback class-weight cap is applied).
    - [ ] Retrain logs include a single structured `window_health` line with order-count coverage, risk prevalence, and dominant regime share.
    - [ ] Unit tests cover skip vs proceed behavior across healthy and unhealthy windows.

- [ ] **NA-R-5: Investigate retrain rejection drivers across data, regime, and deployment gates**
  - Scope: isolate why continuous retraining frequently fails model promotion in shadow runs.
  - Focus areas:
    - [ ] degraded count features
    - [ ] highly skewed risk labels
    - [ ] strong regime concentration
    - [ ] strict deployment sanity checks
  - Acceptance criteria:
    - [ ] Produce a per-window diagnostic table linking each focus area to retrain outcome (`selected=incumbent` vs promoted).
    - [ ] Quantify marginal impact of each focus area on direction loss and composite selection score.
    - [ ] Propose concrete threshold/config updates for each focus area with rollback-safe defaults.
    - [ ] Add an implementation plan that sequences low-risk telemetry first, then gating changes.

- [ ] **FUT-R-1: Evaluate basis/term-structure alpha for hedge timing**
  - Scope: research signal utility from perp basis, funding regime, and spot-perp dislocation for inventory hedging.
  - Acceptance criteria:
    - [ ] Offline report quantifies incremental alpha and drawdown impact.
    - [ ] Candidate features and productionization risk are documented.

- [ ] **FUT-R-2: Research spot/futures coupling for hedging overlay (deferred)**
  - Scope: investigate later-phase coupling where spot and futures can be co-activated for hedge overlays (e.g., basis hedges, inventory neutralization).
  - Acceptance criteria:
    - [ ] Research note defines candidate coupling architectures, risk controls, and failure modes.
    - [ ] Backtest/replay quantifies whether hedge overlay improves drawdown/variance without degrading core alpha.
    - [ ] Output includes go/no-go criteria before any production implementation task is created.
