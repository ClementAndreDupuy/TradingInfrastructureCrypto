# Execution Domain — Position Management & Token Selling

## Overview

Position tracking is centralized in `OrderManager`. Token selling occurs through normal market-making limit orders and automatic stop-limit flattening when losses exceed a threshold.

---

## Common Layout

- `common/connectors/` — connector interfaces, idempotency, and venue order ID bookkeeping
- `common/orders/` — order lifecycle, parent plans, and child scheduling
- `common/portfolio/` — multi-venue inventory state and intent generation
- `common/reconciliation/` — snapshot types and drift detection
- `common/quality/` — adaptive venue quality scoring

## Position Management

### OrderManager (`common/orders/order_manager.hpp`)

- `position_` (double) holds the net position: positive = long, negative = short
- Orders are stored in a **pre-allocated pool of 64 `ManagedOrder` slots** (no heap allocation at runtime)
- Fill events travel through a lock-free `SpscQueue<FillUpdate, 128>`; position mutations happen only on the strategy thread

#### Threading contract
| Thread | Allowed calls |
|---|---|
| Receive (WebSocket/FIX) | `connector_.on_fill` enqueues into SPSC queue only |
| Strategy | `submit()`, `cancel()`, `drain_fills()`, `active_order_count()`, `position()`, `realized_pnl()` |

`drain_fills()` **must** be called at the start of every strategy iteration (before `submit()`). Safe in shadow/single-thread mode — the queue is simply drained inline.

#### Order lifecycle
1. **Submit** — allocates a free slot, assigns `client_order_id`, calls `connector_.submit_order()`
2. **Fill** — receive thread enqueues `FillUpdate`; `drain_fills()` → `apply_fill()` updates qty/price, `position_`, `realized_pnl_`, fires `on_fill` callback
3. **Cancel** — `cancel(client_id)` delegates to the connector

### NeuralAlphaMarketMaker (`market_maker.hpp`)

- `entry_price_` tracks the average entry price for open positions
- Position limits are enforced when posting quotes:
  ```cpp
  bool post_bid = (net_pos + qty) <= cfg_.max_position;
  bool post_ask = (net_pos - qty) >= -cfg_.max_position;
  ```
- **Inventory skew decay** reduces quote skew as position approaches flat to avoid churn:
  ```cpp
  decay = pow(abs(net_pos) / max_position, decay_power)
  ```

---

## Token Selling

### 1. Normal sell orders (market-making)

- Sell orders use `Side::ASK`
- **SmartOrderRouter** (`router/smart_order_router.cpp`) routes them across up to 4 venues: Binance, OKX, Coinbase, Kraken
- Each venue is scored: `base_cost + fill_penalty + queue_penalty + toxicity_penalty + price_penalty`
- Orders can be split across venues if beneficial
- Routing weights adapt to market regime:
  - **High toxicity** (avg > 2.0 bps): prioritizes fill probability
  - **Low fill probability** (avg < 0.45): increases queue weight
  - **High fill probability** (avg > 0.75): reduces weights for faster execution

### 2. Stop-limit flattening (loss closure)

Triggered automatically in `check_stop()` when unrealized loss exceeds `stop_loss_bps`:

```cpp
double unreal_bps = (pos > 0.0)
    ? (mid - entry_price_) / entry_price_ * 1e4
    : (entry_price_ - mid) / entry_price_ * 1e4;

if (unreal_bps < -cfg_.stop_loss_bps) {
    cancel_quotes();  // Pull existing quotes first

    if (pos > 0.0) {
        // Long → SELL: limit below mid by limit_slip_bps
        double limit_px = mid * (1.0 - cfg_.limit_slip_bps * 1e-4);
        stop_id_ = submit_stop_limit(Side::ASK, mid, limit_px, std::abs(pos));
    } else {
        // Short → BUY: limit above mid by limit_slip_bps
        double limit_px = mid * (1.0 + cfg_.limit_slip_bps * 1e-4);
        stop_id_ = submit_stop_limit(Side::BID, mid, limit_px, std::abs(pos));
    }
}
```

On fill: `entry_price_` resets to 0, `stop_id_` clears, and the position is fully flat.

---

## Safety & Reconciliation

| Component | File | Role |
|---|---|---|
| `ReconciliationService` | `common/reconciliation/reconciliation_service.hpp` | Detects position drift vs. exchange snapshots; triggers `RISK_HALT_RECOMMENDED` if mismatch > 1e-6 |
| `IdempotencyJournal` | `common/connectors/idempotency_journal.hpp` | Journals all SUBMIT/CANCEL ops to disk; suppresses duplicate submissions after crashes |
| `VenueOrderMap` | `common/connectors/venue_order_map.hpp` | Bidirectional `client_order_id` ↔ `venue_order_id` mapping (capacity: 512 entries) |
| `cancel_all()` | `common/connectors/exchange_connector.hpp` | Emergency flattening of all positions on a symbol |

### Mismatch classes detected by ReconciliationService
- `MISSING_ORDER` — order exists locally but not on exchange
- `QTY_DRIFT` — quantity mismatch between internal state and exchange
- `FILL_GAP` — fills missing
- `POSITION_DRIFT` — net position mismatch
- `BALANCE_DRIFT` — account balance mismatch

---

## Exchange Connectivity

`ExchangeConnector` (`common/connectors/exchange_connector.hpp`) is the abstract interface:
```cpp
virtual ConnectorResult submit_order(const Order& order) = 0;
virtual ConnectorResult cancel_order(uint64_t client_order_id) = 0;
virtual ConnectorResult cancel_all(const char* symbol) = 0;
virtual ConnectorResult query_order(uint64_t client_order_id, FillUpdate& status) = 0;
```

`LiveConnectorBase` (`common/connectors/live_connector_base.hpp`) implements the shared submission flow:
1. Checks idempotency journal for duplicates
2. Calls venue-specific `submit_to_venue()` with retries
3. Records ACK or FAIL in the journal
4. Updates `VenueOrderMap`

Supported venues: Binance, OKX, Coinbase, Kraken, Shadow (paper trading).
- OKX live execution requires an API passphrase in addition to key/secret, and `cancel_all()` is intentionally unsupported until the strategy layer supplies a documented OKX-wide cancellation flow.

---

## Position State Flow

```
Strategy / NeuralAlphaMarketMaker
    ↓
OrderManager.submit(Order{side: ASK})
    ↓
[Slot allocated in 64-entry pool]
    ↓
SmartOrderRouter → ExchangeConnector.submit_order()
    ↓
[Venue REST API call]
    ↓
FillUpdate received
    ↓
FillUpdate enqueued into SpscQueue (receive thread)
    ↓
OrderManager.drain_fills() → apply_fill() (strategy thread)
    ├→ position_ -= fill_qty        (ASK reduces position)
    ├→ realized_pnl_ updated
    ├→ filled_qty, avg_fill_price updated
    └→ on_fill() callback triggered
    ↓
NeuralAlphaMarketMaker.on_fill()
    ├→ entry_price_ updated (if position remains)
    ├→ bid_id_ / ask_id_ / stop_id_ cleared
    └→ fill event logged
```

---

## Comment Hygiene

- Keep execution comments focused on order-state invariants, venue contracts, and failure handling.
- Remove banner comments, prose summaries of the control flow, and inline comments that only restate nearby code.

## Key Files

| File | Purpose |
|---|---|
| `common/orders/order_manager.hpp` | Core position tracking & order lifecycle |
| `market_maker.hpp` | Quote skewing, stop-limits, position flattening |
| `router/smart_order_router.cpp` | Multi-venue sell routing & scoring |
| `common/reconciliation/reconciliation_service.hpp` | Drift detection & safety halts |
| `common/connectors/live_connector_base.hpp` | Exchange API abstraction with idempotency |
| `common/connectors/venue_order_map.hpp` | Client ↔ venue order ID mapping |
| `common/connectors/exchange_connector.hpp` | Abstract connector interface |
| `common/connectors/idempotency_journal.hpp` | Crash-safe operation journaling |


## Classes & Methods (Quick Reference)

### Strategy & Routing
- **`NeuralAlphaMarketMaker` (`market_maker.hpp`)** — Main quoting strategy with alpha/regime-aware skewing.
  - `on_book_update()`: Runs one strategy cycle (drain fills, read signals, quote/requote, stop checks).
  - `set_alpha_signal(...)`: Injects alpha for deterministic tests/simulation.
- **`SmartOrderRouter` (`router/smart_order_router.hpp`)** — Venue scorer and child-order allocator.
  - `route(...)` / `route_with_alpha(...)`: Produces venue split decisions from quotes and constraints.
  - `infer_regime(...)` / `score_venue_bps(...)`: Derives routing regime and per-venue scoring.
- **`ChildOrderScheduler` (`common/orders/child_order_scheduler.hpp`)** — Converts parent plan urgency/horizon into child execution style.
  - `schedule(...)`: Picks style (passive/IOC/sweep), venue clips, and expected shortfall.
- **`ParentOrderManager` (`common/orders/parent_order_manager.hpp`)** — Stateful parent-plan lifecycle manager.
  - `update_target(...)`: Creates/replaces/updates parent plans from target-position deltas.
  - `on_child_fill()/on_child_cancel()/on_child_reject()`: Reconciles plan progress from child outcomes.

### Order Lifecycle & Connectors
- **`OrderManager` (`common/orders/order_manager.hpp`)** — Owns in-flight orders, fill application, and strategy-visible position.
  - `submit()/cancel()/cancel_all()`: Order lifecycle entry points.
  - `drain_fills()`: Applies queued fills into order slots and position ledger.
  - `ledger_snapshot()`: Exposes aggregated cross-venue position/PnL snapshot.
- **`ExchangeConnector` (`common/connectors/exchange_connector.hpp`)** — Abstract exchange API contract.
  - `connect()/disconnect()/submit/cancel/replace/query/cancel_all/reconcile`: Required connector operations.
- **`LiveConnectorBase` (`common/connectors/live_connector_base.hpp`)** — Shared live connector workflow.
  - `submit_order()/cancel_order()/replace_order()`: Adds idempotency journal + retry wrappers.
  - `query_order()/cancel_all()/reconcile()`: Unified query/cancel/recovery behavior for venues.

### Reliability, Reconciliation, and Portfolio State
- **`IdempotencyJournal` (`common/connectors/idempotency_journal.hpp`)** — Crash-safe op journal.
  - `begin()/ack()/fail()`: Tracks operation lifecycle and duplicate handling.
  - `lookup()/recover()`: Reads existing state to suppress duplicate replays.
- **`VenueOrderMap` (`common/connectors/venue_order_map.hpp`)** — Client↔venue order-id mapping table.
  - `upsert()/get()/erase()/clear()`: Maintains active mappings for cancel/query/replace.
- **`ReconciliationService` (`common/reconciliation/reconciliation_service.hpp`)** — Drift detector across local vs venue/canonical snapshots.
  - `register_connector()/run_periodic_drift_check()/reconcile_on_reconnect()`: Drives reconciliation cycles.
  - `set_canonical_snapshot(...)`: Supplies baseline snapshots for drift classification.
- **`PositionLedger` (`common/portfolio/position_ledger.hpp`)** — Aggregates per-venue inventory, pending qty, and PnL.
  - `on_order_submitted()/on_fill()/on_order_closed()`: Applies lifecycle events into venue/global state.
  - `snapshot()`: Produces unified portfolio snapshot for strategy/reports.
- **`PortfolioIntentEngine` (`common/portfolio/portfolio_intent_engine.hpp`)** — Converts alpha/regime/health into target position intent.
  - `evaluate(...)`: Returns target delta, urgency, flatten flags, and reason codes.
  - `reason_code_to_string(...)`: Maps intent reason enums to log/report labels.
- **`VenueQualityModel` (`common/quality/venue_quality_model.hpp`)** — Adaptive venue-quality estimator.
  - `observe_*` methods: Ingest fill, markout, reject, latency, and health signals.
  - `snapshot()/apply()/persist_snapshot(...)`: Exposes and publishes composite quality penalties.
