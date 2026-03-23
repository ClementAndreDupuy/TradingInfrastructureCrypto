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
