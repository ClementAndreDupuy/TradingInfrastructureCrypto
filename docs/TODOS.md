## TODO List

### CRITICAL

#### [x] 1. Execution engine rebuild
**Status**
- Completed on 2026-03-23.

**Delivered outcome**
- The trading engine now runs the target-position, state-machine, multi-venue execution flow directly in shadow and live modes.
- Adaptive venue quality modelling, reconciliation protections, and post-trade execution attribution are in place.
- Any future execution work should be added here as incremental follow-up items instead of reviving a separate roadmap file.

**Acceptance summary**
- Shadow attribution, target-position planning, parent/child execution, shadow rollout, live cutover, and adaptive venue quality work are complete.
- `AGENTS.md` now points contributors to track follow-up execution work directly in this file.

#### [ ] 2. Regime instability stabilization (ETHUSDT/SOLUSDT shadow follow-up)
**Priority / Severity**
- CRITICAL — regime instability is currently degrading alpha evaluation quality and intent stability.

**Context**
- Recent shadow sessions showed high gating and poor realised IC/ICIR with unstable regime-driven risk posture.
- See implementation plan: `/docs/regime_stability_plan_2026-03-24.md`.

**Execution plan checkpoints**
- [ ] Phase 0: Add retrain cadence, regime churn, and venue-health transition telemetry.
- [ ] Phase 1: Add wall-clock retrain guardrails + startup warm-up gating.
- [ ] Phase 2: Add semantic continuity alignment to reduce label remap churn.
- [ ] Phase 3: Add shock/illiquid hysteresis + persistence in intent decisions.
- [ ] Phase 4: Harden startup book-range behavior to avoid early venue health degradation.
- [ ] Phase 5: Add promotion gates for stability + alpha quality in shadow reports.

**Acceptance criteria**
- Retrain frequency is bounded by wall-clock constraints even under bursty tick throughput.
- Regime switch rate and semantic remap events materially decrease versus baseline runs.
- Hold/flatten oscillation caused by threshold thrash is measurably reduced.
- Startup venue health remains stable for ETHUSDT and SOLUSDT shadow starts.
- Shadow promotion gate reports explicit pass/fail status for stability and quality metrics.
