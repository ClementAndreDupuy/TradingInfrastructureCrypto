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
