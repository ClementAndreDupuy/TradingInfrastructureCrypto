## TODO List

### CRITICAL

#### [ ] 1. Execution engine rebuild roadmap
**Problem**
- The current trading engine still couples signal evaluation to direct order submission instead of trading toward a persistent target position.
- Shadow and live execution need a phased replacement plan with shadow-first validation, concrete acceptance criteria, and examples so future work stays consistent.

**Why this matters**
- Repeated signal-driven entries can create churn, poor exits, and weak attribution even when alpha generation is good.
- The execution redesign will span multiple commits; without a dedicated roadmap it will drift.

**Required work**
- Maintain the detailed phased roadmap in `docs/TODOS_EXECUTION_ENGINE.md`.
- Keep acceptance criteria, examples, and rollout assumptions current as phases evolve.
- Update `AGENTS.md` whenever the roadmap changes.

**Acceptance criteria**
- `docs/TODOS_EXECUTION_ENGINE.md` exists and covers every migration phase from instrumentation through live canary.
- Each phase has clear work items, acceptance criteria, and at least one concrete example.
- `AGENTS.md` points contributors to keep the roadmap and instructions in sync.

