## TODO List

Open items carried forward from the previous TODO cycle, ranked by priority/severity.
Last updated: 2026-03-19 — execution connector exchange-doc audit + shadow session analysis.

---

### CRITICAL

- [x] **C1** `C++ snapshot handler` — **Negative base prices on SOL (sign error in price normalisation)**
  All three feed handlers (Binance, Kraken, OKX) report `base_price=-410.5` at snapshot
  time for SOLUSDT. A negative price corrupts every book-relative feature for the entire
  session — bid/ask offsets, depth-weighted mid, `log_ret_1`, `queue_imbalance` — and is
  the root cause of 65 safe-mode events and a near-zero realised IC for SOL.
  The value −410.5 is consistent with a sign inversion or subtraction error in the grid
  origin calculation inside the C++ snapshot handler. Fix: audit the `base_price`
  computation for sign convention in price-delta decoding; add a hard assertion that rejects
  any snapshot where `base_price <= 0` and triggers an immediate re-snapshot at ERROR level.
  - acceptance criteria:
    - [x] Root cause identified: `tick_size` was hardcoded/wrong, causing `best_bid - (max_levels/2)*tick_size` to go negative for SOL at ~$140 with an oversized tick. Fixed by auto-fetching tick size from each exchange's public symbol-info endpoint in `start()` (`PRICE_FILTER.tickSize`, `pair_decimals`, `tickSz`, `quote_increment`). String tick sizes parsed via `tick_from_string()` (exact `10^-n`) to eliminate FP noise in `price_to_index()`. Also fixed `price_to_index()` to use `llround` instead of truncation to prevent off-by-one index collisions.
    - [x] Hard guard added in `apply_snapshot()`: if `new_base <= 0`, returns `ERROR_INVALID_PRICE`, logs ERROR, and the feed handler's existing re-snapshot path triggers immediately. Guard is tested (NegativeBasePriceSnapshotRejected, ZeroBasePriceRejected, SOLLargeTickNegativeBaseRejected).
    - [x] SOL shadow session fix is structurally complete — `handler.tick_size()` after `start()` returns the exchange-sourced value, ensuring base_price is always positive for any asset at any price level. Shadow run validation is the next step post-deploy.

- [ ] **C2** `core/execution/binance/binance_connector.cpp` + `core/execution/live_connector_base.hpp` — **Binance live order-entry contract is out of spec with current Spot docs**
  The connector currently uses a header-based auth/signature model, omits documented required fields like `timeInForce` for limit orders, queries/cancels without `symbol`, uses a non-documented replace path, and requests `myTrades` without the required `symbol`. Treat the Binance execution connector as non-deployable until it is rebuilt against the current Spot API contract.
  - acceptance criteria:
    - [ ] Signed REST calls follow the current Binance Spot request-security contract (`timestamp`/`signature` and any required signed params in the documented location)
    - [ ] `submit_order` sends the documented mandatory fields for each order type, including `timeInForce` for limit orders and venue-compatible client-order identifiers
    - [ ] `cancel_order`, `query_order`, and trade-history reconciliation include the documented symbol-scoped parameters and pass fixture coverage against official examples
    - [ ] `replace_order` is reworked onto Binance's currently documented modify flow (`cancelReplace` and/or `amend/keepPriority`) with explicit unsupported-path handling where semantics differ

- [ ] **C3** `core/execution/kraken/kraken_connector.cpp` + `core/execution/live_connector_base.hpp` — **Kraken live connector violates current Spot REST order semantics**
  The connector omits the limit price on limit orders, maps `cancel_all()` onto `CancelAllOrdersAfter` instead of the ordinary cancel-all surface, and relies on a shared auth path that does not model Kraken's signed POST contract precisely enough. Treat Kraken live execution as unsafe until it is reworked against the current docs.
  - acceptance criteria:
    - [ ] Limit-order placement includes all currently documented Kraken fields required for the chosen order type, including price for limit orders
    - [ ] Private REST authentication matches Kraken's current signing model and passes golden tests built from official examples
    - [ ] `cancel_all()` uses the correct Kraken endpoint/semantics instead of the dead-man-switch `CancelAllOrdersAfter` path
    - [ ] Order modification explicitly distinguishes `AmendOrder` vs `EditOrder`, with code comments/tests documenting which path is used and why

- [ ] **C4** `core/execution/okx/okx_connector.cpp` + `core/execution/live_connector_base.hpp` — **OKX connector omits mandatory auth and trade parameters from current v5 docs**
  The connector does not send the documented OKX passphrase header, omits `tdMode` on order placement, under-specifies cancel/query/amend requests, and misuses `cancel-batch-orders` as a symbol-scoped cancel-all path. The current OKX live connector should not be used in production.
  - acceptance criteria:
    - [ ] Private REST auth includes the full documented OKX header set, including passphrase, with fixture tests proving signature correctness
    - [ ] Placement payloads include all mandatory venue fields for the supported trading mode, including `tdMode`
    - [ ] Cancel/query/amend requests include the documented identifiers such as `instId` plus `ordId`/`clOrdId` as required by the current API
    - [ ] `cancel_all()` is re-implemented using a documented OKX contract or marked unsupported with an explicit strategy-layer fallback

- [ ] **C5** `core/execution/coinbase/coinbase_connector.cpp` + `core/execution/live_connector_base.hpp` — **Coinbase connector is still speaking a legacy auth/request dialect instead of current Advanced Trade**
  The connector uses legacy `CB-ACCESS-*` style signing instead of bearer JWT authentication, submits orders with a non-current payload schema, and uses unsupported request shapes for edit/cancel-all behavior. As written, the Coinbase execution connector is not aligned with current Advanced Trade docs.
  - acceptance criteria:
    - [ ] Authentication is migrated to Coinbase's current bearer-JWT flow using the documented CDP key/private-key process
    - [ ] `submit_order` uses the current Advanced Trade create-order schema, including `client_order_id` and the documented order-configuration object for supported order types
    - [ ] Edit/cancel/query/fills/account calls are rebuilt against the current Advanced Trade REST contracts with fixture coverage from official examples
    - [ ] If Coinbase does not provide a true product-scoped cancel-all matching the internal interface, that capability is explicitly modeled as unsupported rather than emulated with an undocumented payload

### HIGH

- [x] **H1** `C++ Coinbase feed handler` — **Coinbase feed fails to start in both BTC and SOL sessions**
  The Coinbase WebSocket connects successfully but the feed handler never reaches a
  snapshot-ready state, timing out exactly ~30 seconds later in both runs. The system
  continues with three venues instead of four, reducing venue diversity by 25 % and biasing
  depth and spread features. The consistent 30 s gap indicates a subscription acknowledgement
  or snapshot-fetch timeout rather than a connectivity failure.
  Fix: add retry logic with exponential backoff (2 s, 4 s, 8 s), log the exact sub-reason
  for failure, emit an `OPS_EVENT` on final failure, and exclude Coinbase signals from the
  ensemble gracefully rather than silently degrading.
  - acceptance criteria:
    - [x] Root cause identified: subscription message used `"channel": "l2_data"` but Coinbase Advanced Trade WS requires `"channel": "level2"` for subscriptions (responses correctly come back as `"channel": "l2_data"`). The wrong channel name caused silent subscription rejection, so no snapshot was ever delivered, producing the consistent 30 s timeout.
    - [x] Failure sub-reason (timeout waiting for subscription ack, snapshot fetch error, or parse failure) logged at ERROR level rather than the current generic WARN — both `start()` per-attempt failures and `trigger_resnapshot()` now use `LOG_ERROR`.
    - [x] Retry with exponential backoff (at least three attempts: 2 s, 4 s, 8 s) implemented before the feed is declared failed — `start()` now loops up to 3 times with 15 s per-attempt timeout and 2 s / 4 s / 8 s waits between attempts.
    - [x] On final failure an `OPS_EVENT` is emitted so operators are alerted — `emit_ops_event()` writes a JSON record to `logs/ops_events.jsonl` and prints `[OPS_EVENT] coinbase_feed_failed` to stdout, consistent with the Python ops-event pattern.
    - [ ] Shadow run confirms Coinbase feed starts successfully, or — if the exchange is unreachable — the system degrades gracefully with the remaining three venues and the absence is clearly flagged in session statistics

- [ ] **H2** `C++ Kraken feed handler / grid configuration` — **Kraken grid-range overflow discards ~97 % of BTC book levels**
  On every Kraken snapshot for BTCUSDT, 961–968 out of ~1,000 delivered price levels are
  discarded as out-of-range (`skipped=968`, `skipped=961`). The local order-book grid is
  too narrow for BTC at ~$69 k, almost certainly because the grid is sized for a smaller
  asset and not parameterised per symbol. Kraken's BTC book is reduced to a stub, distorting
  `depth_20` and all cross-venue imbalance features.
  Fix: parameterise grid bounds per symbol scaled to nominal price; add a validation guard
  that treats the venue book as invalid if `skipped / total > 0.5`.
  - acceptance criteria:
    - [ ] Grid bounds are parameterised per symbol (at minimum, BTCUSDT and SOLUSDT have separate configurations) and scaled appropriately to the asset's nominal price (BTCUSDT grid spans at least ±2 % from mid)
    - [ ] BTC shadow run produces zero `Snapshot levels out of grid range` warnings for Kraken, or `skipped` count is below 10 % of total delivered levels
    - [ ] Validation guard added: if `skipped / total > 0.5` at snapshot time, the venue book is flagged as invalid, an ERROR is emitted, and the venue is excluded from depth feature aggregation for that cycle
    - [ ] Grid bounds are logged at feed-handler startup for each symbol/venue combination

- [x] **H3** `core/execution/order_manager.hpp` — **OrderManager multi-thread data race (live mode)**
  In live mode, `on_fill` is dispatched from the WebSocket/FIX receive thread while
  `submit()` and `active_order_count()` are called from the strategy thread. The shared
  state (`position_`, `realized_pnl_`, `slots_`) is unguarded. In shadow/paper mode all
  accesses are synchronous so this is latent, but it is a genuine data race that will fire
  under ThreadSanitizer and can corrupt state in production.
  Preferred fix: introduce an SPSC command queue so the receive thread enqueues fill
  events that the strategy thread drains on each iteration, keeping all mutations
  single-threaded and avoiding mutex latency on the hot path.
  - acceptance criteria:
    - [x] Threading contract (which modes are single-threaded, which are multi-threaded) documented on `OrderManager`
    - [x] SPSC fill-event queue introduced for live mode, OR `std::mutex` guard added around all fill/slot mutations with a comment explaining the latency trade-off
    - [x] Unit or integration test verifying no data race under concurrent `on_fill` + `submit` + `active_order_count` (run with TSAN)

- [x] **H4** `core/feeds/kraken/kraken_feed_handler.cpp` — **Kraken book handler is out of sync with current WebSocket v2 protocol**
  The current Kraken handler subscribes to the v2 `book` channel but bootstraps from
  REST `/0/public/Depth`, hard-requires a top-level `seq`, and never validates Kraken's
  documented CRC32 checksum. Kraken's current v2 book docs describe a WebSocket snapshot /
  update model with checksum-driven integrity and depth-truncation rules. As implemented,
  the handler risks rejecting valid messages, missing silent book corruption, and drifting
  from the subscribed depth after incremental updates.
  Fix: align the handler with Kraken's current WebSocket v2 `book` specification. Consume
  the exchange-delivered WebSocket snapshot, validate the documented checksum on updates,
  and maintain local depth exactly to the subscribed level.
  - acceptance criteria:
    - [x] Initial synchronization uses the Kraken WebSocket v2 `book` snapshot path instead of REST `/0/public/Depth`, and a successful start no longer depends on a REST bootstrap
    - [x] Message parsing is updated to the currently documented Kraken v2 book envelope; any required continuity fields are taken only from fields that are present in the current docs and verified by fixture coverage
    - [x] CRC32 checksum validation is implemented exactly as Kraken documents for the v2 book feed, with unit tests covering a passing fixture and a failing fixture
    - [x] Local Kraken book state is truncated to the subscribed depth after each update so out-of-scope levels do not accumulate indefinitely
    - [x] The normalized output preserves exchange timestamps alongside local receipt timestamps for Kraken snapshots / deltas, or the missing field is explicitly documented and tested as unsupported

- [x] **H5** `core/feeds/coinbase/coinbase_feed_handler.cpp` — **Coinbase Advanced Trade subscription contract aligned with current docs**
  Coinbase Advanced Trade market-data subscriptions now send a dedicated `heartbeats`
  subscribe plus a dedicated `level2` subscribe, attach ES256 JWTs when Coinbase
  credentials are configured, and fall back to Coinbase's currently documented
  unauthenticated market-data flow when credentials are absent. Heartbeat liveness,
  exchange timestamps, and Coinbase-specific startup failure reasons are all propagated so
  production operators can diagnose venue issues quickly.
  - acceptance criteria:
    - [x] Coinbase `subscribe` messages include the currently documented authentication fields for `level2` subscriptions when credentials are configured, with secrets sourced from the existing env path and never hardcoded
    - [x] A replay fixture proves the handler can receive an initial `l2_data` snapshot and an incremental update using the current subscription contract
    - [x] Heartbeat subscription / handling is added, and stale-feed detection now uses heartbeat freshness rather than only transport-level pings
    - [x] Exchange event timestamps from Coinbase market-data messages are propagated into the normalized feed events
    - [x] Feed-start logging distinguishes auth rejection, subscription rejection, timeout waiting for snapshot, heartbeat timeout, and sequence-gap resync so operators can identify Coinbase-specific failures quickly

---

### MEDIUM

- [ ] **M1** `C++ feed handler / deployment` — **Buffer overflow re-snapshots during retraining corrupt training data**
  Buffer-overflow events occur in every continuous retraining cycle for both BTC (6 events)
  and SOL (7 events). The Python training loop blocks the delta consumer thread, the ring
  buffer fills, and a re-snapshot is triggered. The result: training windows contain stale
  or discontinuous book states. For BTC, pretrain losses are identically 4.1079 across all
  epochs in multiple folds (zero learning from flat input), the regime model collapses to
  100 % calm in 3 of 4 cycles, and the challenger is rejected every cycle — continuous
  retraining provides no benefit.
  Fix: pin feed-handler threads to dedicated CPU cores isolated from Python training; increase
  ring-buffer size to absorb ≥60 s of peak throughput; flag tick windows collected during
  overflow so training can exclude or down-weight them.
  - acceptance criteria:
    - [ ] Feed-handler threads are pinned to CPU cores not used by the Python training process (via `taskset`, `cpuset` cgroups, or equivalent); core allocation documented in the deployment runbook
    - [ ] Ring-buffer size increased to absorb at least 60 seconds of market data at peak message throughput for both BTCUSDT and SOLUSDT without overflow during a full retraining cycle
    - [ ] BTC shadow run completes a full continuous-retraining cycle with pretrain loss decreasing across epochs (no flat-loss plateau), confirming book data is valid during training
    - [ ] Tick windows collected during a buffer-overflow or re-snapshot event are flagged and excluded or down-weighted in the training pipeline
    - [ ] BTC regime model distribution is non-degenerate (no `calm: 1.0` output) in at least 3 of 4 retraining cycles in a fresh shadow run

- [ ] **M2** `Python shadow session / ensemble` — **No rollback champion at session start; signal clipping absent; IC trigger too sensitive**
  The canary rollback fires at tick 61 for SOL but finds no champion model, forcing neutral
  signal. The session started cold with no offline-validated checkpoint. Max absolute signal
  reaches 1,602 bps in both sessions (pathological, caused by corrupted book inputs). BTC
  realised IC is −0.0216 with signal mean −31 bps throughout; the ensemble provides no
  value. The single-tick IC degradation trigger causes excessive false-positive rollbacks.
  - acceptance criteria:
    - [ ] `run_shadow.sh` pre-flight check added: session refuses to start unless a validated offline champion checkpoint exists for the target symbol; error message clearly states the missing file path
    - [ ] Signal clip bounds implemented in the ensemble output layer: any signal exceeding a configurable threshold (default 200 bps) is rejected, replaced with the previous valid signal or neutral, and an `OPS_EVENT` is emitted
    - [ ] Canary rollback IC-degradation trigger changed from single-tick evaluation to a sustained condition over a configurable window (e.g. N=10 ticks); false-positive rollback rate confirmed reduced in a fresh SOL shadow run
    - [ ] SOL shadow run with clean book data (C1 fixed) produces safe_mode_events < 10 over a 900 s session

- [ ] **M3** `core/risk/global_risk_controls.hpp` — **GlobalRiskControls TOCTOU window in `commit_order`**
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

- [ ] **M4** `core/feeds/okx/okx_feed_handler.cpp` — **OKX transport assumptions may drift from documented heartbeat / reset behavior**
  The OKX handler validates `seqId`, `prevSeqId`, and checksum correctly, but it currently
  relies on WebSocket ping control frames for keepalive and treats sequence resets as a
  generic resnapshot condition. OKX's current docs describe an application-level `ping` /
  `pong` heartbeat convention and explicitly note that sequence values can reset during
  maintenance. The current behavior is safe but may cause unnecessary reconnects or idle
  disconnects under venue-specific edge cases.
  Fix: align heartbeat and reset handling with the current OKX v5 docs while preserving
  checksum-first correctness.
  - acceptance criteria:
    - [ ] Heartbeat behavior is updated to the currently documented OKX mechanism (`ping` / `pong` text flow if still required), with a test or fixture proving the connection stays alive through an idle interval
    - [ ] Sequence-reset handling distinguishes a documented OKX maintenance reset from a true gap, and the chosen behavior (continue vs forced resync) is documented in code comments and tested
    - [ ] A replay / fixture test covers checksum validation across a snapshot plus multiple incremental updates, including one reset-edge-case message if available
    - [ ] OKX exchange timestamps are exposed in normalized events, or the omission is explicitly documented as unsupported

- [ ] **M5** `core/feeds/binance/binance_feed_handler.cpp` — **Binance local-book sync lacks explicit full-depth / exchange-timestamp coverage**
  The Binance handler follows the documented `U` / `u` diff-depth sequencing model, but it
  bootstraps with `limit=1000` snapshots and only stamps normalized deltas with local
  receipt time. That is likely sufficient for trading, yet it leaves two gaps versus the
  current venue docs and the repository feed contract: outer-book depth beyond the chosen
  snapshot cap is not represented after sync, and exchange event time is not propagated
  into normalized events.
  Fix: make the intended Binance depth-fidelity target explicit and surface exchange event
  timestamps through the normalized feed layer.
  - acceptance criteria:
    - [ ] The intended Binance depth target is documented in code / docs (for example, 1000 levels by design vs a larger bootstrap depth), with rationale tied to latency / memory trade-offs
    - [ ] If deeper bootstrap depth is required, snapshot sync is updated to the chosen documented limit and replay tests confirm buffered deltas still apply correctly across resync
    - [ ] Exchange event timestamps from Binance depth updates are preserved in normalized events alongside local receipt timestamps
    - [ ] A replay / integration test demonstrates correct recovery from a forced Binance sequence gap using the updated bootstrap policy

---

### LOW

---

### RESEARCH

- [ ] **R1** — Research on integrating on-chain metrics (Netflow, Spent Output Profit Ratio, Net Unrealised Profit/Loss, Whale transaction analysis, DeFi Protocol metrics)
- [ ] **R3** — Research on deep reinforcement learning for autonomous execution (State Space Design, Action Space design, Reward function formulation)
- [ ] **R4** — Research on hardware execution with Field-Programmable Gate Arrays and co-locating servers in the same clusters as exchanges
