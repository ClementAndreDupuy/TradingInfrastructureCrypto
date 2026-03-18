## TODO List

Open items carried forward from the previous TODO cycle, ranked by priority/severity.
Last updated: 2026-03-18 — core domain audit.

---

### CRITICAL
- _No open critical items._

---

### HIGH

- [ ] **H8** `core/execution/order_manager.hpp` — **OrderManager multi-thread data race (live mode)**
  In live mode, `on_fill` is dispatched from the WebSocket/FIX receive thread while
  `submit()` and `active_order_count()` are called from the strategy thread. The shared
  state (`position_`, `realized_pnl_`, `slots_`) is unguarded. In shadow/paper mode all
  accesses are synchronous so this is latent, but it is a genuine data race that will fire
  under ThreadSanitizer and can corrupt state in production.
  Preferred fix: introduce an SPSC command queue so the receive thread enqueues fill
  events that the strategy thread drains on each iteration, keeping all mutations
  single-threaded and avoiding mutex latency on the hot path.
  - acceptance criteria:
    - [ ] Threading contract (which modes are single-threaded, which are multi-threaded) documented on `OrderManager`
    - [ ] SPSC fill-event queue introduced for live mode, OR `std::mutex` guard added around all fill/slot mutations with a comment explaining the latency trade-off
    - [ ] Unit or integration test verifying no data race under concurrent `on_fill` + `submit` + `active_order_count` (run with TSAN)

---

### MEDIUM

- [ ] **M4** `core/risk/global_risk_controls.hpp` — **GlobalRiskControls TOCTOU window in `commit_order`**
  `commit_order` calls `check_order` to validate limits then separately applies the
  atomic adds. A second concurrent `commit_order` can pass the same `check_order` gate
  before either commit writes back, causing both to succeed and the aggregate exposure to
  exceed the configured cap by up to one order notional. Latent with the current
  single-strategy design but must be resolved before concurrent strategy instances share
  the same `GlobalRiskControls`.
  Fix: replace the separate check+add sequence with a compare-exchange loop on each
  limit counter so the check and the update are a single atomic operation. The existing
  `atomic_add` helper needs a bounded-CAS variant that returns `false` (rejected) if the
  post-add value would exceed the cap, rolling back on failure.
  - acceptance criteria:
    - [ ] `atomic_add` helper extended with a bounded-CAS variant (`atomic_add_bounded`) that rejects atomically if the post-add value would exceed the cap
    - [ ] `commit_order` refactored to use `atomic_add_bounded` on all four counters (gross notional, net notional, venue, symbol) with no separate `check_order` pre-pass
    - [ ] Unit test verifying that concurrent `commit_order` calls never cause any counter to exceed its configured cap (run with TSAN and/or explicit concurrent threads)

---

### LOW
- _No open low items._

---

### RESEARCH

- [ ] **R1** — Research on integrating on-chain metrics (Netflow, Spent Output Profit Ratio, Net Unrealised Profit/Loss, Whale transaction analysis, DeFi Protocol metrics)
- [ ] **R3** — Research on deep reinforcement learning for autonomous execution (State Space Design, Action Space design, Reward function formulation)
- [ ] **R4** — Research on hardware execution with Field-Programmable Gate Arrays and co-locating servers in the same clusters as exchanges
