# core/execution/

Execution stack for order lifecycle, routing/scheduling, portfolio state, connector reliability, and reconciliation.

## Common Layout

- `common/connectors/` — connector interfaces, idempotency journal, venue order-id mapping.
- `common/orders/` — order manager + parent/child execution planning.
- `common/portfolio/` — ledger snapshots and portfolio intent engine.
- `common/reconciliation/` — reconciliation snapshot types and drift service.
- `common/quality/` — adaptive venue quality model.
- `router/` — smart order router logic.
- `market_maker.hpp` — neural alpha market-making strategy surface.

## Classes & Methods (Quick Reference)

### Strategy & Routing
- **`NeuralAlphaMarketMaker` (`market_maker.hpp`)**
  - `on_book_update()`: Runs one strategy iteration (signals, quote management, stop handling).
  - `set_alpha_signal(...)`: Injects deterministic alpha/risk values for tests.

- **`SmartOrderRouter` (`router/smart_order_router.hpp`)**
  - `route(...)` / `route_with_alpha(...)`: Builds routing decisions and child allocations.
  - `infer_regime(...)`: Derives scoring regime from venue inputs.
  - `score_venue_bps()/expected_shortfall_bps(...)`: Computes venue ranking costs.

- **`ChildOrderScheduler` (`common/orders/child_order_scheduler.hpp`)**
  - `schedule(...)`: Chooses child execution style and venue clips for an active parent plan.

- **`ParentOrderManager` (`common/orders/parent_order_manager.hpp`)**
  - `update_target(...)`: Creates/updates/replaces parent execution plans from target deltas.
  - `on_child_fill()/on_child_cancel()/on_child_reject()`: Reconciles plan state from child events.

### Order Lifecycle & Connectors
- **`OrderManager` (`common/orders/order_manager.hpp`)**
  - `submit()/cancel()/cancel_all()`: Entry points for order lifecycle actions.
  - `drain_fills()`: Applies queued fills and updates local position + ledger state.
  - `ledger_snapshot()/active_order_count()`: Returns strategy-visible state.

- **`ExchangeConnector` (`common/connectors/exchange_connector.hpp`)**
  - `connect()/disconnect()/submit_order()/cancel_order()/replace_order()/query_order()/cancel_all()/reconcile()`: Base connector contract.

- **`LiveConnectorBase` (`common/connectors/live_connector_base.hpp`)**
  - `submit_order()/cancel_order()/replace_order()`: Shared idempotent + retrying connector flow.
  - `query_order()/cancel_all()/reconcile()`: Shared query/cancel/recovery operations.
  - `fetch_reconciliation_snapshot(...)`: Venue extension point for reconciliation snapshots.

- **`IdempotencyJournal` (`common/connectors/idempotency_journal.hpp`)**
  - `begin()/ack()/fail()`: Tracks op lifecycle and crash-recovery states.
  - `lookup()/recover()`: Reads historical state to avoid duplicate operations.

- **`VenueOrderMap` (`common/connectors/venue_order_map.hpp`)**
  - `upsert()/get()/erase()/clear()`: Maintains client↔venue order-id mappings.

### Portfolio, Reconciliation, and Venue Quality
- **`PositionLedger` (`common/portfolio/position_ledger.hpp`)**
  - `on_order_submitted()/on_fill()/on_order_closed()`: Applies order lifecycle events to inventory state.
  - `snapshot()`: Produces aggregate global + per-venue position/PnL snapshot.

- **`PortfolioIntentEngine` (`common/portfolio/portfolio_intent_engine.hpp`)**
  - `evaluate(...)`: Converts alpha/regime/venue-health/ledger state into target-position intent.
  - `reason_code_to_string(...)`: Converts intent enums to stable log labels.

- **`ReconciliationService` (`common/reconciliation/reconciliation_service.hpp`)**
  - `register_connector()/run_periodic_drift_check()/reconcile_on_reconnect()`: Runs reconciliation cycles.
  - `set_canonical_snapshot(...)/set_canonical_snapshot_fetcher(...)`: Supplies baseline snapshot source.
  - `is_quarantined()/state_for(...)`: Exposes venue reconciliation status.

- **`VenueQualityModel` (`common/quality/venue_quality_model.hpp`)**
  - `observe_fill_probability()/observe_markout()/observe_reject()/observe_cancel_latency()/observe_health()`: Ingests execution-quality observations.
  - `snapshot()/apply()/persist_snapshot(...)`: Exposes and persists adaptive venue-quality outputs.
