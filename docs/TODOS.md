## TODO List

---

### HIGH

#### [ ] 1. Feed bootstrap ordering and bridge wiring correctness
**Problem**
- Feed bootstrap can still regress if any engine path starts handlers before the LOB publisher, `BookManager`, and feed callbacks are fully wired.
- The exact failure mode is subtle: the WebSocket connects successfully, but the initial synchronized snapshot never reaches the bridge, so venue health logic can treat a live feed as dead.
- Book grids also depend on venue tick-size refresh happening before `BookManager` construction.

**Why this matters**
- Shadow mode relies on the same C++ feed/bootstrap path as live mode for book state and bridge publication.
- Missing the bootstrap snapshot corrupts the first observable state for downstream consumers.
- Regressing startup ordering would silently break venue readiness without an obvious parser error.

**Required work**
- Keep `LobPublisher`, `BookManager`, callback registration, and `start()` ordering locked down in every engine/bootstrap entrypoint.
- Add regression coverage for feed start -> snapshot callback -> bridge publish ordering.
- Ensure venue tick sizes are refreshed before constructing `BookManager` grids.
- Audit any future shadow/live startup surface for the same ordering contract.

**Acceptance criteria**
- A started feed publishes its first synchronized snapshot into `BookManager` and the LOB bridge without waiting for a later delta.
- Engine startup tests fail if callbacks or publisher wiring happen after `start()`.
- Book grids use exchange-derived tick sizes when metadata fetch succeeds.


#### [ ] 2. Execution engine rebuild roadmap
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

