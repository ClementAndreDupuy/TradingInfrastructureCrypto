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

### HIGH

#### [ ] 3. LOB publisher neural model data enrichment

**Context**
- `core/ipc/lob_publisher.hpp` currently publishes only top-5 price/size levels with a single timestamp and no trade flow data.
- Analysis confirmed the current `LobSlot` is insufficient for short-horizon neural alpha generation.
- See analysis in session that identified the five gaps below.

---

#### [x] 3a. Trade flow data (aggressor direction + recent volume)

**Priority / Severity**
- HIGH — single biggest missing signal; virtually every published microstructure alpha paper uses trade direction.

**Scope**
- `core/common/types.hpp`: Add `TradeFlow` struct (`last_trade_price`, `last_trade_size`, `recent_traded_volume`, `trade_direction` where `0`=buy/lifted-ask, `1`=sell/hit-bid, `255`=unknown).
- `core/ipc/lob_publisher.hpp`: Add `last_trade_price`, `last_trade_size`, `recent_traded_volume`, `trade_direction` fields to `LobSlot`. Update `publish()` signature to accept a `TradeFlow`.
- `core/feeds/common/book_manager.hpp`: Add `TradeCallback` + `trade_handler()`, maintain `last_trade_flow_` state and rolling 1-second volume accumulator. Pass to `publish_lob()`.
- `core/engine/feed_bootstrap.hpp`: Wire `feed.set_trade_callback(book.trade_handler())` in `wire_book_bridge_and_callbacks`.
- All four feed handlers (hpp + cpp): Add `TradeCallback trade_callback_` member and `set_trade_callback()`. Subscribe to respective trade streams and call `trade_callback_` on each fill event:
  - Binance: switch WS path to combined stream (`/stream?streams={sym}@depth@100ms/{sym}@aggTrade`), parse `aggTrade` events (`m` field determines direction).
  - OKX: extend subscription JSON to include `{"channel":"trades","instId":"..."}`, parse `trades` channel messages (`side` field determines direction).
  - Coinbase: add `market_trades` channel to subscription messages, parse `market_trades` events (`side` field determines direction).
  - Kraken: add `{"method":"subscribe","params":{"channel":"trade",...}}` message, parse `trade` channel updates (`side` field determines direction).
- `research/neural_alpha/runtime/core_bridge.py`: Update `SLOT_FMT` and `read_new_ticks()` to expose `last_trade_price`, `last_trade_size`, `recent_traded_volume`, `trade_direction`.
- `tests/unit/ipc_test.cpp`: Add test asserting trade flow fields are written and read back correctly.

**Acceptance criteria**
- `LobSlot` contains `last_trade_price`, `last_trade_size`, `recent_traded_volume`, and `trade_direction`.
- Each feed handler subscribes to the exchange trade stream in addition to the depth stream without disrupting existing depth/snapshot behaviour.
- `trade_direction` is `0` (buy) or `1` (sell) for every real fill event; never `255` unless the feed genuinely does not provide aggressor-side information.
- `recent_traded_volume` is a rolling 1-second sum of `last_trade_size` values, reset each second.
- `CoreBridge.read_new_ticks()` returns dicts containing all four trade-flow keys.
- All existing unit tests (`ipc_test`, `binance_feed_test`, `okx_feed_test`, `coinbase_feed_test`, `kraken_feed_test`) continue to pass.
- A new `ipc_test` case verifies: publish a slot with known trade-flow values, read the mmap file directly, assert the fields match.

---

#### [x] 3b. Increase LOB depth from 5 to 10 levels

**Priority / Severity**
- HIGH — 5 levels loses iceberg context and depth asymmetry beyond the near touch.

**Scope**
- `core/ipc/lob_publisher.hpp`: Expand `bid_price`, `bid_size`, `ask_price`, `ask_size` arrays from `[5]` to `[10]`. Update `k_slot_size` constant and `static_assert`. Update publish loop bound from `5` to `10`.
- `core/feeds/common/book_manager.hpp`: Change `book_.get_top_levels(5, ...)` to `get_top_levels(10, ...)`. Update `pub_bids_.reserve(10)` and `pub_asks_.reserve(10)`.
- `research/neural_alpha/runtime/core_bridge.py`: Update `SLOT_FMT` to use `10d` instead of `5d` for bid/ask arrays. Update `read_new_ticks()` loop to `range(10)` and expose `bid_price_1`…`bid_price_10` etc.
- `tests/unit/ipc_test.cpp`: Update all byte-offset assertions in `SlotContentsMatchPublishedData` and `BookManagerSnapshotPublishesImmediately` for the new layout. Update ask-price offset comment.

**Acceptance criteria**
- `sizeof(LobSlot) == k_slot_size` static assert passes with new size.
- `SlotContentsMatchPublishedData` test verifies best bid at the correct new byte offset and best ask at the correct new byte offset.
- `CoreBridge` exposes 10 bid and 10 ask price/size fields per tick dict.
- Publishing with fewer than 10 actual levels fills trailing slots with `0.0` without crash or UB.

---

#### [ ] 3c. Order count per level

**Priority / Severity**
- MEDIUM — number of resting orders is a distinct signal from aggregate size (1×100-lot vs 100×1-lot).

**Scope**
- `core/common/types.hpp`: Add `uint32_t order_count = 0` to `PriceLevel`. Add `uint32_t order_count = 0` to `Delta`. Keep checksum computation over `price` + `size` only (exchange checksums do not include order count).
- `core/orderbook/orderbook.hpp`: Add `bid_order_counts_` and `ask_order_counts_` (`vector<uint32_t>`, size `max_levels`). Add matching scratch buffers. Populate from `level.order_count` in `apply_snapshot` and from `delta.order_count` in `apply_delta`. Return `order_count` in `get_top_levels`. Shift order-count arrays alongside size arrays in `recenter_grid`.
- `core/ipc/lob_publisher.hpp`: Add `uint32_t bid_order_count[10]` and `uint32_t ask_order_count[10]` to `LobSlot`. Write them in `publish()` from the `PriceLevel::order_count` field.
- Feed handlers: Populate `Delta::order_count` where the exchange provides it:
  - OKX `books` channel level format is `[price, size, deprecated_count, order_count]` — read index `[3]`.
  - Binance, Coinbase, Kraken depth streams do not provide per-level order count — leave at `0`.
- `research/neural_alpha/runtime/core_bridge.py`: Update `SLOT_FMT` to include `10I` for bid order counts and `10I` for ask order counts. Expose `bid_oc_1`…`bid_oc_10` and `ask_oc_1`…`ask_oc_10` in returned dicts.
- `tests/unit/ipc_test.cpp`: Add case that publishes levels with non-zero order counts and reads them back from mmap.

**Acceptance criteria**
- `PriceLevel` and `Delta` carry `order_count`; existing callers that do not set it default to `0` with no API break.
- `OrderBook::get_top_levels` returns the correct `order_count` for each level after a snapshot and after a delta.
- `recenter_grid` shifts order-count arrays correctly — no stale counts left at wrong price levels after recentering.
- OKX levels populated from the `books` channel carry non-zero `order_count` when the exchange sends it.
- `LobSlot` byte layout is verified by updated `SlotContentsMatchPublishedData` test.

---

#### [ ] 3d. Dual timestamps (exchange vs. local)

**Priority / Severity**
- MEDIUM — exchange-to-local latency delta is a congestion/adverse-selection signal.

**Scope**
- `core/ipc/lob_publisher.hpp`: Replace `int64_t timestamp_ns` in `LobSlot` with `int64_t timestamp_exchange_ns` and `int64_t timestamp_local_ns`. Update `publish()` signature to accept both (already present separately in `Delta` and `Snapshot`).
- `core/feeds/common/book_manager.hpp`: Pass both timestamps. In `snapshot_handler`, pass `s.timestamp_exchange_ns` and `s.timestamp_local_ns`. In `delta_handler`, pass `d.timestamp_exchange_ns` and the computed local wall time.
- `research/neural_alpha/runtime/core_bridge.py`: Update `SLOT_FMT` (`qq` instead of `q` for the timestamp pair). Expose both `timestamp_exchange_ns` and `timestamp_local_ns` in returned dicts.
- `tests/unit/ipc_test.cpp`: Update `SlotContentsMatchPublishedData` to verify both timestamp fields at their new byte offsets. Update `BookManagerSnapshotPublishesImmediately` timestamp assertion to read `timestamp_local_ns`.

**Acceptance criteria**
- `LobSlot` contains both `timestamp_exchange_ns` and `timestamp_local_ns` at the correct byte offsets.
- When a delta arrives with `timestamp_exchange_ns == 0` (exchange did not provide it), the field is `0` in the slot; `timestamp_local_ns` is always a valid non-zero wall-clock value.
- `CoreBridge` exposes both timestamp keys per tick dict.
- All existing timestamp-related assertions in `ipc_test.cpp` pass with updated offsets.

---

#### [ ] 3e. Book update rate (events per second)

**Priority / Severity**
- MEDIUM — event frequency is a proxy for short-term volatility and microstructure activity.

**Scope**
- `core/ipc/lob_publisher.hpp`: Add `uint32_t events_per_second` field to `LobSlot`. Update `publish()` signature to accept it.
- `core/feeds/common/book_manager.hpp`: Add a rolling 1-second event counter (`event_count_window_`, `window_start_steady_ns_`, `last_events_per_second_`). On each `publish_lob` call: if more than 1 second has elapsed since `window_start_steady_ns_`, store `event_count_window_` into `last_events_per_second_` and reset the counter; otherwise increment. Pass `last_events_per_second_` to `publish()`.
- `research/neural_alpha/runtime/core_bridge.py`: Update `SLOT_FMT` to include the `uint32_t` field. Expose `events_per_second` in returned dicts.
- `tests/unit/ipc_test.cpp`: Add a test that publishes multiple slots rapidly and asserts `events_per_second` is non-zero after the counter window elapses.

**Acceptance criteria**
- `events_per_second` in the slot reflects the number of `publish_lob` calls in the prior 1-second window.
- Value is `0` on the very first slot (no complete window yet) and increases as ticks arrive.
- Counter resets cleanly when no updates arrive for more than 1 second (next publish starts a fresh window).
- `CoreBridge` exposes `events_per_second` in the tick dict.
