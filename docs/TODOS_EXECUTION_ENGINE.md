# Execution Engine Rebuild TODOs

Purpose: replace the current signal-to-submit loop with a target-position, state-machine, multi-venue execution engine while preserving shadow-first validation.

## Operating Rules
- Update this file whenever any execution-engine phase, acceptance criteria, interface, or rollout assumption changes.
- Update `AGENTS.md` in the same commit whenever this file changes so future agents follow the latest plan.
- Keep examples concrete and tied to observable logs, metrics, or tests.
- Do not mark a phase complete unless every acceptance criterion is demonstrably satisfied.

---

## Phase 0 — Baseline instrumentation and diagnostics
**Priority:** Critical  
**Goal:** Measure why the current execution loop overtrades, misses exits, or loses edge before changing behaviour.

**Work items**
- Add shadow execution attribution for decision price, fill price, edge at entry, implementation shortfall, hold time, and venue-level markout.
- Log parent intent metadata: signal, risk, target position, urgency, expected cost, and reason for entry/exit/hold.
- Extend shadow session summary with fee drag, spread paid/captured, average inventory age, fill-to-markout, and per-venue contribution.
- Produce one comparison report from an existing shadow run that explains whether losses came from fees, slippage, churn, or stale inventory.

**Acceptance criteria**
- [ ] A shadow run emits enough metrics to explain a negative net PnL without reading raw order logs manually.
- [ ] The end-of-run summary includes per-venue execution quality, not only fill counts and net PnL.
- [ ] At least one replay or shadow report attributes losses into fees, slippage, and adverse-selection buckets.

**Example**
- Example summary line: `net_pnl=-0.05 gross_alpha=0.14 fees=-0.06 spread_paid=-0.04 markout=-0.09 avg_hold_ms=820 venue_worst=OKX`.
- Example decision log: `intent=enter_long target_pos=0.60 current_pos=0.10 urgency=AGGRESSIVE expected_edge_bps=7.2 max_shortfall_bps=2.5`.

---

## Phase 1 — Position ledger
**Priority:** Critical  
**Goal:** Introduce a central multi-venue ledger that knows exactly what is owned, where, for how long, and at what average cost.

**Work items**
- Add a `PositionLedger` for symbol-level and venue-level inventory, average entry price, realized/unrealized PnL, pending orders, and inventory age.
- Feed the ledger from shadow/live fills and top-of-book mid-price updates.
- Expose a stable snapshot interface consumable by the engine loop, router, and shadow summary.
- Add tests covering partial fills, partial exits, cross-venue inventory, and inventory aging.

**Acceptance criteria**
- [ ] Ledger global position matches aggregate connector position across all venues within tolerance.
- [ ] Venue-level inventory survives partial fills/exits and never requires inference from logs.
- [ ] Tests cover long entry, partial unwind, full flatten, and venue divergence cases.

**Example**
- Example snapshot: `global_pos=0.75 | OKX=0.50 avg=87.54 age_ms=600 | KRAKEN=0.25 avg=87.55 age_ms=420`.

---

## Phase 2 — Portfolio intent engine
**Priority:** Critical  
**Goal:** Convert alpha + risk + regime + current inventory into a target position and urgency rather than direct order submits.

**Work items**
- Add a `PortfolioIntentEngine` that consumes alpha, regime, current portfolio state, venue health, and expected costs.
- Compute `target_global_position`, `position_delta`, `urgency`, `flatten_now`, and `max_shortfall_bps`.
- Support long-only mode first; short-enabled logic can remain feature-flagged.
- Log why each intent changed: stronger alpha, alpha decay, negative reversal, illiquid regime, stale inventory, etc.

**Acceptance criteria**
- [ ] No direct entry/exit orders are created from raw signal thresholds alone in the new path.
- [ ] Identical signal inputs produce deterministic target-position outputs in unit tests.
- [ ] The engine can explain every state change via machine-readable reason codes.

**Example**
- Example: `signal_bps=8.0 risk=0.20 size_fraction=0.80 expected_cost=2.0 => target_pos=0.64 urgency=BALANCED`.
- Example: `signal_bps=-4.5 current_pos=0.50 regime_shock=0.10 => target_pos=0.00 urgency=AGGRESSIVE reason=negative_reversal`.

---

## Phase 3 — Parent execution plans
**Priority:** High  
**Goal:** Keep persistent execution objectives instead of firing isolated child orders each loop.

**Work items**
- Add a `ParentOrderManager` that owns a working plan: side, total qty, remaining qty, deadline, urgency, and execution style permissions.
- Connect plan progress to fills, cancels, rejects, and expiry.
- Support plan replacement when target position changes meaningfully.
- Add tests for fill progression, cancellation, replacement, and deadline expiry.

**Acceptance criteria**
- [ ] A target-position change creates or updates one parent execution plan, not ad hoc child orders every loop.
- [ ] Remaining quantity and plan state are always observable.
- [ ] Plans expire or escalate when their deadline passes.

**Example**
- Example: `plan_id=17 side=BID total_qty=0.40 remaining=0.15 urgency=PASSIVE deadline_ms=1500`.

---

## Phase 4 — Child order scheduler and router upgrade
**Priority:** High  
**Goal:** Turn the current SOR into a stateful execution scheduler that chooses venue, style, and urgency-aware order placement.

**Work items**
- Add a `ChildOrderScheduler` that selects between passive join, passive improve, marketable IOC, and sweep.
- Refactor `SmartOrderRouter` into a lower-level scorer used by the scheduler.
- Include venue quality, queue risk, fill probability, toxicity, latency, and inventory-age penalties.
- Make the scheduler horizon-aware so short-lived alpha trades more aggressively than long-lived alpha.

**Acceptance criteria**
- [ ] Execution style depends on urgency and horizon rather than a fixed IOC-style submit path.
- [ ] Venue selection is based on expected shortfall, not just displayed best price.
- [ ] Unit tests verify that high-toxicity or low-fill venues are deprioritized.

**Example**
- Example: `urgency=PASSIVE horizon_ticks=500 => post on KRAKEN best queue-adjusted venue`.
- Example: `urgency=AGGRESSIVE horizon_ticks=10 => IOC on OKX + BINANCE sweep up to 0.30 qty`.

---

## Phase 5 — Shadow-only state machine rollout
**Priority:** High  
**Goal:** Replace the current shadow trading loop with the new target-position engine while keeping legacy logic available for A/B comparison.

**Work items**
- Gate the new path behind config flags and run it only in shadow first.
- Preserve a legacy shadow mode so replay and shadow sessions can compare old vs new behaviour on identical feed/alpha inputs.
- Add a state machine with at least: `FLAT`, `ENTERING`, `HOLDING`, `REDUCING`, `FLATTENING`, `HALTED`.
- Expand shadow reports with before/after comparisons for churn, shortfall, and realized edge capture.

**Acceptance criteria**
- [ ] New shadow mode can run end-to-end without manual intervention.
- [ ] A/B comparison on the same replay data shows equal or lower churn and equal or better net alpha capture.
- [ ] State transitions are logged and testable.

**Example**
- Example transition: `HOLDING -> REDUCING reason=alpha_decay current_pos=0.55 target_pos=0.20`.

---

## Phase 6 — Live canary rollout
**Priority:** Medium  
**Goal:** Enable the new engine in live mode under strict safety controls after shadow validation passes.

**Work items**
- Limit rollout by symbol, venue set, and max position.
- Add kill-switch hooks for reconciliation mismatch, venue disconnect clusters, and shortfall breaches.
- Record live-vs-shadow divergence metrics and automated rollback triggers.
- Keep the old live path behind a fast rollback flag until the canary is stable.

**Acceptance criteria**
- [ ] Live canary can be disabled instantly through config without code changes.
- [ ] Reconciliation or venue-health failures force flatten-or-halt behaviour within configured limits.
- [ ] Live execution quality remains within pre-defined tolerance of shadow benchmarks for the same regime bucket.

**Example**
- Example rollout: `symbol=BTCUSDT venues=OKX,KRAKEN max_position=0.10 engine=new_execution_v1`.

---

## Phase 7 — Adaptive venue quality model
**Priority:** Medium  
**Goal:** Continuously re-estimate venue quality so routing reflects actual realized markout and fill quality.

**Work items**
- Add a `VenueQualityModel` with rolling fill probability, passive/taker markout, reject rate, cancel latency, and venue health penalties.
- Feed its outputs into the scheduler/router scoring path.
- Bound adaptation speed to avoid unstable oscillations.
- Persist summary snapshots for post-trade analysis.

**Acceptance criteria**
- [ ] Venue scores change in response to measured execution outcomes, not hard-coded constants alone.
- [ ] Routing decisions remain stable under noisy short-term conditions.
- [ ] Reports show why a venue gained or lost priority over time.

**Example**
- Example: `OKX passive_markout_100ms=-1.8bps => reduce passive posting weight; KRAKEN fill_prob=0.82 => increase passive preference`.

---

## Definition of Done for the full program
A new execution engine rollout is complete only when all of the following are true:
- Shadow runs explain PnL decomposition without raw-log forensics.
- The new engine trades toward target position rather than repeatedly buying on raw signal thresholds.
- Shadow replay A/B results show lower churn and better or equal fee-adjusted PnL.
- Live canary completes without reconciliation drift, orphan venue positions, or uncontrolled inventory aging.
- `AGENTS.md` and this file describe the same phased plan and operating rules.
