## TODO List

Open items carried forward from the previous TODO cycle, ranked by priority/severity.
Last updated: 2026-03-18 — core domain audit added A1–A2.

---

### AUDIT (core domain — 2026-03-18)

- [ ] **A1** — **OrderManager multi-thread data race (live mode)**
  **File**: `core/execution/order_manager.hpp`
  **Severity**: High — potential data race in production.
  **Detail**: `position_`, `realized_pnl_`, and the `slots_` array are plain
  (non-atomic) member variables. In shadow/paper mode all accesses are
  synchronous so this is safe. In live mode, `on_fill` is dispatched from
  the WebSocket/FIX receive thread while `submit()` and `active_order_count()`
  are called from the strategy thread — the required serialisation is only
  documented on `ExchangeConnector`, not enforced in `OrderManager` itself.
  **Recommended fix**: either (a) document the threading contract explicitly
  on `OrderManager` and add a `std::mutex` guard around fill/slot mutations in
  live mode, or (b) introduce an SPSC command queue so the WebSocket thread
  enqueues fill events that the strategy thread drains on each iteration,
  keeping all mutations single-threaded. Option (b) is preferred to avoid
  mutex latency on the hot path.

- [ ] **A2** — **GlobalRiskControls TOCTOU window in `commit_order`**
  **File**: `core/risk/global_risk_controls.hpp`
  **Severity**: Medium — concurrent commits could transiently exceed limits.
  **Detail**: `commit_order` calls `check_order` to validate limits, then
  separately applies the atomic adds. A second concurrent `commit_order` can
  pass the same `check_order` gate before either commit writes back, causing
  both to succeed and the aggregate exposure to exceed the configured cap by
  up to one order notional.
  **Recommended fix**: Replace the separate check+add sequence with a
  compare-exchange loop on each limit counter so the check and the update are
  a single atomic operation. The existing `atomic_add` helper needs to be
  extended with a bounded-CAS variant that returns `false` (rejected) if the
  post-add value would exceed the cap, rolling back on failure. This requires
  restructuring the four counters (gross, net, venue, symbol) to each use
  such bounded CAS. Given that the system is currently designed for a single
  strategy thread on the hot path this race is latent, but must be resolved
  before concurrent strategy instances share the same `GlobalRiskControls`.

---

### RESEARCH

- [ ] **R1** — Research on integrating on-chain metrics (Netflow, Spent Output Profit Ratio, Net Unrealised Profit/Loss, Whale transaction analysis, DeFi Protocol metrics)
- [ ] **R3** — Research on deep reinforcement learning for autonomous execution (State Space Design, Action Space design, Reward function formulation)
- [ ] **R4** — Research on hardware execution with Field-Programmable Gate Arrays and co-locating servers in the same clusters as exchanges
