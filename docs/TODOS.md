# Project TODOs

### CRITICAL
- [x] **FUT-BN-1: Add Binance USDⓈ-M futures connector skeleton (REST signing + endpoint routing)**
  - Scope: create `core/execution/binance/binance_futures_connector.{hpp,cpp}` built on `LiveConnectorBase`, with futures-specific base URL (`/fapi`) and signed request builder.
  - Acceptance criteria:
    - Connector compiles and links in the existing CMake target.
    - `submit_order`, `cancel_order`, `replace_order`, `query_order`, and `cancel_all` route to Binance futures endpoints only (no `/api/v3`).
    - HMAC signature/timestamp/recvWindow behavior is covered by deterministic unit tests.

- [x] **FUT-BN-2: Futures order model + side semantics for long/short**
  - Scope: extend execution order schema to represent futures position direction safely (e.g., `position_side`, `reduce_only`, `close_position`, `time_in_force`, `working_type`).
  - Acceptance criteria:
    - Long/short intent can be represented without overloading spot fields.
    - Binance hedge-mode and one-way-mode payload mapping is explicit and unit-tested.
    - Invalid combinations (e.g., `close_position=true` with quantity) are rejected pre-flight with deterministic error codes.

- [x] **FUT-BN-3: Exchange filters + pre-trade validation for futures contracts**
  - Scope: ingest futures `exchangeInfo` filters (tick size, step size, min notional, trigger constraints), enforce in hot path before REST calls.
  - Acceptance criteria:
    - Validation rejects malformed orders before network submission.
    - Quantity/price normalization obeys contract precision and does not violate filter bounds.
    - Unit tests cover edge cases for BTCUSDT and at least one alt perpetual.

- [x] **FUT-BN-4: Position and leverage state reconciliation**
  - Scope: add futures reconciliation snapshot fetchers for positions, open orders, and account risk; wire into `ReconciliationService` and `PositionLedger`.
  - Acceptance criteria:
    - Reconciliation detects drift for both net and hedge-mode positions.
    - Local ledger is corrected after reconnect without duplicate fills.
    - Quarantine path is triggered when drift exceeds configured threshold.

### HIGH

- [x] **RISK-ACC-1: Venue-aware capital accounting + live session PnL ledger**
  - Scope: add per-venue starting equity configuration, wire reconciled free balances into pre-trade affordability checks, and produce live-mode end-of-session PnL/equity summary aligned with shadow metrics schema.
  - Acceptance criteria:
    - Runtime config supports per-venue starting equity and minimum free-collateral buffers for BINANCE/KRAKEN/OKX/COINBASE.
    - Order submission is blocked or clipped when venue free balance/collateral cannot fund intended order + fee buffer.
    - Live engine emits periodic and shutdown metrics for per-venue and global `{start_equity, end_equity, realized_pnl, unrealized_pnl, fees, net_pnl, return_pct}`.
    - Drawdown checks are driven by the live session accounting PnL source and validated by unit/integration tests.

- [x] **FUT-BN-5: Funding-rate and mark-price risk gates**
  - Scope: add risk checks that can block/scale orders based on projected funding cost, mark/index divergence, and max leverage per symbol.
  - Acceptance criteria:
    - Risk module can reject new exposure when leverage or maintenance margin thresholds are breached.
    - Funding-aware sizing path is configurable via `config/` and covered by unit tests.
    - Rejections expose stable reason codes for observability.

- [x] **FUT-BN-6: Connector failure-injection + live contract tests for futures**
  - Scope: mirror existing connector tests for futures endpoints and error taxonomy (auth, rate limit, timestamp skew, reduce-only violation).
  - Acceptance criteria:
    - New futures test suite validates operation state invariants in failure paths.
    - Existing spot connector tests remain unchanged and passing.
    - Replayable deterministic fixtures are added for all expected Binance error classes.

- [x] **FUT-BN-7: Runtime config + kill-switch integration for futures venues**
  - Scope: introduce futures connector config (base URLs, recvWindow, hedge mode, leverage caps) and ensure global kill switch and cancel-all semantics include futures open orders.
  - Acceptance criteria:
    - Futures connector can be enabled/disabled per environment without code changes.
    - Kill switch issues futures `cancelAllOpenOrders` and blocks fresh submits until reset.
    - Startup validation fails fast on missing futures credentials/config keys.


- [x] **FUT-BN-11: Validate spot-orderbook alpha execution on futures (basis + latency controls)**
  - Scope: support strategy mode where alpha is derived from spot L2/trade-flow signals while execution occurs on Binance perpetual futures.
  - Acceptance criteria:
    - Signal pipeline explicitly tags source venue/instrument (`spot`) and execution venue/instrument (`futures`) for every decision.
    - Pre-trade guardrails bound allowable spot-perp basis divergence and stale-signal latency before submitting futures orders.
    - Backtest/replay shows PnL attribution split between alpha edge and basis slippage, with fail-safe downgrade when basis regime breaks.

- [x] **FUT-BN-12: Runtime strategy-mode switch (spot-only portfolio vs futures-only long/short)**
  - Scope: add explicit runtime mode selection so the engine can run either (A) spot-only portfolio management flow or (B) futures-only long/short flow, with no mixed execution in a single mode.
  - Acceptance criteria:
    - Startup config supports a single required enum mode: `spot_only` or `futures_only`.
    - In `spot_only`, all futures connectors/orders are disabled at compile/runtime boundaries and risk checks validate spot-only assumptions.
    - In `futures_only`, spot execution paths are disabled and futures position/risk checks (long/short, leverage, margin) are mandatory.
    - Mode transitions require controlled restart and emit an auditable configuration event.

- [x] **FUT-BN-13: Portfolio intent engine futures long/short mapping from alpha**
  - Scope: extend `PortfolioIntentEngine`/strategy intent generation so alpha sign and confidence map deterministically to futures long/short/flat targets per symbol, including reduce/flip behavior for existing exposure.
  - Detailed flow (alpha read -> target position):
    1. Read normalized alpha (`signal_bps`, `size_fraction`, `risk_score`) and current position state (qty, side, leverage headroom, margin mode, health flags).
    2. Apply gating in fixed order: kill-switch / venue health -> risk-off/regime blocks -> staleness checks -> basis/funding guards (when enabled).
    3. Convert alpha sign + magnitude into a signed target exposure:
       - `signal_bps > +entry_bps` => positive target (long),
       - `signal_bps < -entry_bps` => negative target (short),
       - `|signal_bps| <= deadband_bps` => flat hold/flatten policy.
    4. Apply sizing multipliers (confidence, risk score, regime scale, health scale) and clamp by max position, leverage, and collateral.
    5. Resolve transition plan from current to target (`add`, `reduce`, `flatten`, `flip`) with explicit order sequencing for `long -> short` and `short -> long`.
    6. Emit execution intents with futures direction fields (`position_side`, `reduce_only`, `close_position`) and auditable reason codes.
  - Acceptance criteria:
    - Positive alpha generates long intent, negative alpha generates short intent, and near-zero alpha resolves to flat/no-trade via configurable deadband.
    - Target-position transitions (`long -> short`, `short -> long`, `long/short -> flat`) are explicit and risk-checked before order emission.
    - Generated intents include futures-specific direction metadata (`position_side`, `reduce_only`/close semantics) so execution does not infer direction from spot fields.
    - Unit/integration tests validate deterministic outcomes for all transition classes (`flat->long`, `flat->short`, `long->short`, `short->long`, `long->flat`, `short->flat`) under representative alpha/regime/risk inputs.

- [x] **FUT-BN-14: Shadow runtime futures config surface (`run_shadow` + `futures.yaml`)**
  - Scope: add explicit shadow runtime configuration for futures execution mode so operators can run spot-only or futures-only shadow sessions without ad-hoc CLI args.
  - Acceptance criteria:
    - Add `config/shadow/futures.yaml` with required schema (`enabled`, `strategy_mode`, `position_mode`, `default_leverage`, per-symbol leverage caps, deadband/flip controls).
    - Extend `config/shadow/runtime.yaml` with `strategy_mode` (`spot_only` / `futures_only`) and `futures_config` path.
    - Update `deploy/run_shadow.sh` to read these keys, validate combinations, and pass deterministic engine args/config wiring.
    - Startup fails fast with clear diagnostics if `strategy_mode=futures_only` and futures config/credentials are missing.
    - Shadow startup logs include resolved strategy mode + futures config path for auditability.

### MEDIUM
- [ ] **FUT-BN-8: Shadow-mode rollout plan for Binance futures**
  - Scope: shadow execution path for futures with no live order placement, plus metrics for slippage, reject rate, and reconciliation drift.
  - Acceptance criteria:
    - Shadow mode can run as a dedicated session with futures trading explicitly activated (`futures_enabled=true`) while final order placement side effects remain disabled.
    - Shadow mode reuses production code paths except final submit side effects.
    - Daily report includes futures-specific health metrics and drift deltas.
    - Go-live checklist defines promotion criteria and rollback conditions.

- [ ] **FUT-BN-9: Observability and audit trail for futures lifecycle**
  - Scope: structured logs + metrics for futures request/response latencies, error buckets, and order lifecycle transitions.
  - Acceptance criteria:
    - Metrics include per-endpoint p50/p95/p99 latency and reject taxonomy.
    - Audit logs can correlate `client_order_id` to Binance futures order IDs and reconciliation events.
    - No secrets appear in logs.

### LOW
- [ ] **FUT-BN-10: Multi-Assets Mode and portfolio-margin compatibility review**
  - Scope: document compatibility matrix for one-way vs hedge mode, single-asset vs multi-assets mode, and future portfolio-margin migration.
  - Acceptance criteria:
    - Supported combinations are explicitly documented with constraints.
    - Unsupported account modes fail with clear startup diagnostics.

### RESEARCH
- [ ] **FUT-R-1: Evaluate basis/term-structure alpha for hedge timing**
  - Scope: research signal utility from perp basis, funding regime, and spot-perp dislocation for inventory hedging.
  - Acceptance criteria:
    - Offline report quantifies incremental alpha and drawdown impact.
    - Candidate features and productionization risk are documented.

- [ ] **FUT-R-2: Research spot/futures coupling for hedging overlay (deferred)**
  - Scope: investigate later-phase coupling where spot and futures can be co-activated for hedge overlays (e.g., basis hedges, inventory neutralization).
  - Acceptance criteria:
    - Research note defines candidate coupling architectures, risk controls, and failure modes.
    - Backtest/replay quantifies whether hedge overlay improves drawdown/variance without degrading core alpha.
    - Output includes go/no-go criteria before any production implementation task is created.
