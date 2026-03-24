# core/

C++ hot-path runtime components for trading, routing, risk, and shared-memory bridges.

## Active component map

- `common/` — shared enums/types/utilities.
- `orderbook/` — in-memory L2 order book state.
- `feeds/` — exchange market-data handlers + normalized book updates.
- `ipc/` — mmap bridges (`AlphaSignalReader`, `RegimeSignalReader`, `LobPublisher`).
- `risk/` — runtime guardrails (`CircuitBreaker`, global limits, recovery guard).
- `execution/` — connector stack, order lifecycle, routing/scheduling, portfolio/reconciliation.
- `engine/` / `shadow/` — orchestration and paper-trading surfaces.

## Core Classes & Methods (Quick Reference)

### `orderbook/`
- **`OrderBook`** — Owns the L2 price-grid state and applies normalized snapshots/deltas.
  - `apply_snapshot(const Snapshot&)`: Reinitializes book state from a full snapshot with validation.
  - `apply_delta(const Delta&)`: Applies a single level update into bid/ask grids.
  - `get_best_bid()/get_best_ask()/get_mid_price()`: Returns top-of-book derived prices.

### `feeds/`
- **`BinanceFeedHandler` / `KrakenFeedHandler` / `OkxFeedHandler` / `CoinbaseFeedHandler`** — Venue-specific snapshot+stream synchronizers.
  - `start()/stop()`: Controls feed lifecycle and synchronization loop.
  - `process_message(const std::string&)`: Parses venue payloads and emits normalized updates.
  - `refresh_tick_size()`: Fetches tick-size metadata used when constructing books.
- **`BookManager`** — Adapter from feed callbacks into `OrderBook` + freshness tracking.
  - `snapshot_handler()/delta_handler()`: Returns callbacks that apply incoming updates.
  - `age_ms()`: Returns elapsed time since last accepted update.

### `ipc/`
- **`AlphaSignalReader`** — Reads mmap alpha frames via seqlock retry logic.
  - `open()/close()/read()`: Manages mapping and returns latest consistent alpha signal.
  - `allows_long()/allows_short()/allows_mm()`: Applies alpha/risk/staleness gating helpers.
- **`RegimeSignalReader`** — Reads mmap regime probabilities.
  - `read()`: Returns latest consistent regime frame.
  - `is_stale(...)`: Validates freshness against caller threshold.
- **`LobPublisher`** — Single-writer mmap ring publisher for top-of-book snapshots.
  - `open()/close()/is_open()`: Manages shared-memory ring mapping.
  - `publish(...)`: Writes one top-5 LOB snapshot slot.

### `risk/`
- **`CircuitBreaker`** — Runtime guardrails for order/message rates, drawdown, and stale/abnormal markets.
  - `check_order_rate()/check_message_rate()`: Enforces throughput limits.
  - `check_drawdown()/check_book_age()/check_price_deviation()`: Detects and reacts to unsafe states.
  - `record_leg_result(...)`: Tracks loss streaks and escalation.
- **`GlobalRiskControls`** — Portfolio/venue/symbol notional limit checks.
  - `check_order(...)`: Evaluates whether an order would breach limits.
  - `commit_order(...)`: Commits exposure state after successful check.
- **`RecoveryGuard`** — Recovery path sanity guard.
  - `check(...)`: Trips protection if recovery counters exceed configured caps.
- **`RiskConfigLoader`** — Config parser for `RiskRuntimeConfig`.
  - `load(path, out)`: Loads risk runtime settings from file.

### `execution/`
- **`ExchangeConnector`** — Abstract connector interface.
  - `submit_order/cancel_order/replace_order/query_order/cancel_all/reconcile`: Required connector operations.
- **`LiveConnectorBase`** — Shared live connector behavior (journal, retry, order-id mapping).
  - `submit_order()/cancel_order()/replace_order()`: Wraps venue calls with recovery semantics.
  - `query_order()/cancel_all()/reconcile()`: Shared query/cancel/recovery entry points.
- **`OrderManager`** — Strategy-side order lifecycle + fill application.
  - `submit()/cancel()/cancel_all()`: Places/cancels orders through connector.
  - `drain_fills()`: Applies queued fills and updates position/ledger state.
- **`NeuralAlphaMarketMaker`** — Book-driven quoting with alpha/regime inputs and stop handling.
  - `on_book_update()`: Main strategy loop step.
  - `set_alpha_signal(...)`: Injects alpha/risk for tests.
- **`SmartOrderRouter`** — Venue scoring and child-order allocation.
  - `route()/route_with_alpha(...)`: Produces child routing decisions.
  - `score_venue_bps()/expected_shortfall_bps(...)`: Venue cost/shortfall scoring.
