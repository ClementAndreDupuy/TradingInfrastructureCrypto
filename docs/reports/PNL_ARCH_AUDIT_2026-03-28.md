# PnL / Capital / Session Accounting Audit — 2026-03-28

## Scope

Reviewed hot-path execution/risk and shadow-session accounting to verify:

1. whether start capital is modeled per venue,
2. whether order sizing is constrained by venue cash/balance,
3. whether end-of-session net PnL is produced in live and shadow modes,
4. what is currently logged and what is missing.

## What exists today

### 1) PnL calculation in core execution/shadow

- `PositionLedger` tracks per-venue position, average entry, realized and unrealized PnL from fills + mark updates. This is strategy-side inventory accounting, not account-equity accounting. 
- `ShadowConnector` maintains its own per-venue fill/cashflow/fees/position state and computes:
  - realized PnL,
  - unrealized PnL,
  - total fees,
  - net cashflow,
  - total PnL = (realized - fees) + unrealized.
- `ShadowEngine` aggregates those venue-level quantities and emits a session summary (`Shadow session summary`) at shutdown.

### 2) Logging that exists

- Shadow JSONL logging includes per-order and per-fill details, including cumulative cashflow, cumulative realized PnL, fee, implementation shortfall, and markout.
- Live engine logs portfolio intent decisions and venue-quality snapshots.
- Live engine does **not** emit a final session PnL/equity summary comparable to shadow mode.

### 3) Risk controls that exist

- `GlobalRiskControls` enforces notional caps:
  - global gross/net,
  - per-venue gross notional,
  - cross-venue net notional,
  - symbol concentration.
- This is exposure-limit control, not balance/equity budgeting.

## Confirmed gaps (matching your concern)

### Gap A — No start capital per venue in core runtime

There is no configuration or runtime field for `starting_capital_by_venue` in the C++ live/shadow execution path.

- `RiskRuntimeConfig` and `RiskConfigLoader` parse risk/notional and fee knobs, but no venue capital map.
- `config/live/risk.yaml` and `config/shadow/risk.yaml` do not define start-equity/cash buckets per venue.

### Gap B — Order sizing is not constrained by available money per venue

Order submission path gates on notional limits and circuit checks, but not on free balances:

- The submit path uses `GlobalRiskControls::commit_order(...)` before routing to venue connectors.
- There is no pre-trade check that compares order cost against per-venue available quote/base balances.
- Reconciliation fetches balances from venues, but those balances are used for drift/comparison workflows; they are not wired into sizing limits.

### Gap C — Live mode does not compute authoritative net PnL for end-of-session

- `trading_engine_main.cpp` calls `shadow_engine.log_summary()` only in shadow mode.
- In live mode there is no analogous end-of-session aggregate with realized/unrealized/fees/cashflow/equity.
- Live portfolio snapshot builder currently aggregates position quantities from reconciliation snapshots; it does not aggregate a PnL ledger.

### Gap D — Drawdown guard in live main loop is currently fed with stale value

In `trading_engine_main.cpp`, drawdown check passes `circuit_breaker.realized_pnl()` back into `check_drawdown(...)`. That value is initialized to zero and only changes when `check_drawdown(...)` is called with a new realized PnL source. In the current loop, no source-of-truth realized PnL is injected, so drawdown effectively evaluates against stale self-state.

## Secondary observations

- `PortfolioIntentEngine` sizes against `max_position` and alpha/risk/regime/venue-health factors; it does not consume account balance/equity by venue.
- Research backtest has `initial_capital_usd`, but that is in Python offline evaluation and not connected to live hot-path venue account state.

## Recommended implementation plan

### Phase 1 — Introduce venue-capital model and pre-trade affordability checks

1. Add runtime config blocks:
   - `capital.starting_equity_usd_by_venue` (static baseline)
   - `capital.min_free_collateral_buffer_usd_by_venue` (safety buffer)
2. Extend runtime account state to track latest reconciled balances per venue.
3. Add `can_afford_order(exchange, side, qty, px, fees)` check in pre-submit path.
4. Reject/clip orders when free balance (or free collateral) is insufficient.

### Phase 2 — Add live PnL/equity session ledger

1. Build a live session accounting component that merges:
   - fills,
   - fees,
   - realized PnL,
   - mark-to-market unrealized PnL,
   - venue transfers/deposits/withdrawals (if available).
2. Emit periodic structured metrics + final session summary in live mode, mirroring shadow report fields.
3. Publish both global and per-venue values:
   - start equity,
   - end equity,
   - net PnL,
   - return %, drawdown, fees.

### Phase 3 — Fix drawdown source wiring

1. Feed circuit breaker drawdown check with session-accounting realized PnL (not internal CB copy).
2. Add tests asserting drawdown trigger when session PnL breaches threshold.

## Suggested success criteria

- For every submitted order, engine can answer: "which venue balance/collateral budget allowed this order?"
- End-of-session reports exist for both live and shadow modes with identical schema.
- Drawdown protections trigger from real realized PnL values, not stale placeholders.
