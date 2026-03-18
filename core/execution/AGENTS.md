# Execution Domain — Position Management & Token Selling

## Overview

Position tracking is centralized in `OrderManager`. Token selling occurs through normal market-making limit orders and automatic stop-limit flattening when losses exceed a threshold.

---

## Position Management

### OrderManager (`order_manager.hpp`)

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
- **SmartOrderRouter** (`smart_order_router.cpp`) routes them across up to 4 venues: Binance, OKX, Coinbase, Kraken
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
| `ReconciliationService` | `reconciliation_service.hpp` | Detects position drift vs. exchange snapshots; triggers `RISK_HALT_RECOMMENDED` if mismatch > 1e-6 |
| `IdempotencyJournal` | `idempotency_journal.hpp` | Journals all SUBMIT/CANCEL ops to disk; suppresses duplicate submissions after crashes |
| `VenueOrderMap` | `venue_order_map.hpp` | Bidirectional `client_order_id` ↔ `venue_order_id` mapping (capacity: 512 entries) |
| `cancel_all()` | `exchange_connector.hpp` | Emergency flattening of all positions on a symbol |

### Mismatch classes detected by ReconciliationService
- `MISSING_ORDER` — order exists locally but not on exchange
- `QTY_DRIFT` — quantity mismatch between internal state and exchange
- `FILL_GAP` — fills missing
- `POSITION_DRIFT` — net position mismatch
- `BALANCE_DRIFT` — account balance mismatch

---

## Exchange Connectivity

`ExchangeConnector` (`exchange_connector.hpp`) is the abstract interface:
```cpp
virtual ConnectorResult submit_order(const Order& order) = 0;
virtual ConnectorResult cancel_order(uint64_t client_order_id) = 0;
virtual ConnectorResult cancel_all(const char* symbol) = 0;
virtual ConnectorResult query_order(uint64_t client_order_id, FillUpdate& status) = 0;
```

`LiveConnectorBase` (`live_connector_base.hpp`) implements the shared submission flow:
1. Checks idempotency journal for duplicates
2. Calls venue-specific `submit_to_venue()` with retries
3. Records ACK or FAIL in the journal
4. Updates `VenueOrderMap`

Supported venues: Binance, OKX, Coinbase, Kraken, Shadow (paper trading).

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

## Key Files

| File | Purpose |
|---|---|
| `order_manager.hpp` | Core position tracking & order lifecycle |
| `market_maker.hpp` | Quote skewing, stop-limits, position flattening |
| `smart_order_router.cpp` | Multi-venue sell routing & scoring |
| `reconciliation_service.hpp` | Drift detection & safety halts |
| `live_connector_base.hpp` | Exchange API abstraction with idempotency |
| `venue_order_map.hpp` | Client ↔ venue order ID mapping |
| `exchange_connector.hpp` | Abstract connector interface |
| `idempotency_journal.hpp` | Crash-safe operation journaling |
