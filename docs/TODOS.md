# Project TODOs

> Completed items are archived in `docs/reports/TODOS_ARCHIVE_*.md`.  
> Last cleaned: 2026-03-31 — portfolio long/short audit findings added.

---

## CRITICAL

- [x] **PORT-C1: Fix live config YAML key mismatch — regime thresholds all zero at runtime**
  - Source: `AUDIT_PORTFOLIO_LONG_SHORT_2026-03-31.md` / CRITICAL-1
  - Files: `config/live/portfolio.yaml`, `core/engine/algo_config_loader.hpp:123–127`, `core/engine/trading_engine_main.cpp`
  - Problem: `config/live/portfolio.yaml` uses legacy key names (`shock_flatten_threshold`, `illiquid_reduce_threshold`) that `AlgoConfigLoader::load_portfolio` never reads. The five fields `shock_enter_threshold`, `shock_exit_threshold`, `illiquid_enter_threshold`, `illiquid_exit_threshold`, `regime_persistence_ticks` remain zero. With `persistence = 0` the regime hysteresis activates on the very first tick and oscillates every 500 ms, generating aggressive flatten orders continuously while any position is open.
  - Acceptance criteria:
    - [x] `config/live/portfolio.yaml` contains all five keys with explicit values matching intended live thresholds.
    - [x] `PortfolioIntentConfig portfolio_cfg;` declaration in `trading_engine_main.cpp` is changed to `PortfolioIntentConfig portfolio_cfg{};` (zero-initialize before load).
    - [x] A startup assertion (or loader validation) rejects a zero `shock_enter_threshold` or `regime_persistence_ticks` and aborts with a clear error message before entering the main loop.
    - [x] Unit test added that constructs `PortfolioIntentEngine` with live config values and verifies shock/illiquid regime does **not** activate when `p_shock = 0.05` and `p_illiquid = 0.05`.

- [x] **PORT-C2: `oldest_inventory_age_ms` never populated in live mode — stale inventory exit inoperative**
  - Source: `AUDIT_PORTFOLIO_LONG_SHORT_2026-03-31.md` / CRITICAL-2
  - Files: `core/engine/trading_engine_main.cpp:247–288` (`build_live_portfolio_snapshot`), `core/execution/common/portfolio/portfolio_intent_engine.hpp:120–121`
  - Problem: `build_live_portfolio_snapshot` sums positions from reconciliation but never sets `oldest_inventory_age_ms`, which stays zero. The stale-inventory condition `0 >= stale_inventory_ms` is permanently false, rendering the 5-second forced-exit in `config/live/portfolio.yaml` completely dead in live mode. Shadow mode is unaffected.
  - Acceptance criteria:
    - [x] Live mode populates `oldest_inventory_age_ms` in `PositionLedgerSnapshot` — either by switching to `PositionLedger::snapshot()` (preferred) or by extending `build_live_portfolio_snapshot` to carry age from reconciliation data.
    - [x] Integration test verifies that a position held beyond `stale_inventory_ms` in live/reconciliation mode triggers `STALE_INVENTORY` reason and a non-zero `position_delta` targeting flat.
    - [x] Inventory age is included in the `portfolio intent` log line for observability.

---

## HIGH

- [x] **PORT-H1: `edge_positive` gate not applied to short entries — misleading `ALPHA_NEGATIVE` reason code**
  - Source: `AUDIT_PORTFOLIO_LONG_SHORT_2026-03-31.md` / HIGH-1
  - File: `core/execution/common/portfolio/portfolio_intent_engine.hpp:126, 154–175`
  - Problem: Long entry requires `signal_bps > expected_cost_bps` (`edge_positive`); short entry does not. When `|signal_bps| < expected_cost_bps` for a short, `alpha_scale` zeros out the magnitude (correct, no position taken), but `ALPHA_NEGATIVE` is still appended as the reason code. Consumers of logs and metrics will believe a short entry was warranted. For longs the equivalent condition correctly emits `ALPHA_DECAY`.
  - Acceptance criteria:
    - [ ] `negative_entry` requires `std::abs(alpha_signal.signal_bps) > expected_cost_bps` in addition to the existing threshold check.
    - [ ] When `|signal| <= expected_cost` for a short, reason code is `ALPHA_DECAY`, not `ALPHA_NEGATIVE`.
    - [ ] Unit tests cover: (a) short entry with sufficient edge → `ALPHA_NEGATIVE` + negative target; (b) short signal below cost → `ALPHA_DECAY` + zero target; (c) symmetry with equivalent long cases.

- [x] **PORT-H2: `AlphaSignalReader` members `fd_` and `ptr_` not initialized in constructor**
  - Source: `AUDIT_PORTFOLIO_LONG_SHORT_2026-03-31.md` / HIGH-2
  - File: `core/ipc/alpha_signal.hpp:28–31, 134–138`
  - Problem: Constructor initializes `path_`, `signal_min_bps_`, `risk_max_` but leaves `fd_` and `ptr_` uninitialized. The destructor calls `close()` which checks `fd_ >= 0` — undefined behaviour if `open()` was never called, potentially invoking `::close()` on a live file descriptor belonging to another component.
  - Acceptance criteria:
    - [ ] `fd_` is declared with in-class default `= -1`; `ptr_` is declared with in-class default `= nullptr`.
    - [ ] No other behavior changed.
    - [ ] Existing `AlphaSignalReader` unit/IPC tests continue to pass.

- [x] **PORT-H3: `allows_long()` / `allows_short()` fail-open with no observable warning when IPC unavailable**
  - Source: `AUDIT_PORTFOLIO_LONG_SHORT_2026-03-31.md` / HIGH-3
  - File: `core/ipc/alpha_signal.hpp:97–113`
  - Problem: When `ptr_ == nullptr` (shared-memory file absent or `open()` failed), both methods return `true`, allowing all trades without any signal gate. `SmartOrderRouter::route_with_alpha` relies on these gates. No log warning is emitted, so the operator has no indication that ungated order flow is occurring.
  - Acceptance criteria:
    - [ ] A throttled `LOG_WARN` (at most once per N seconds, configurable) is emitted from `allows_long()` and `allows_short()` when `!ptr_`.
    - [ ] The decision to fail-open vs fail-closed is explicitly documented in a comment adjacent to the `if (!ptr_) return true` lines.
    - [ ] Unit test verifies the warning fires exactly once per throttle window when reader is not opened.

- [x] **PORT-H4: No symmetric `positive_reversal` exit for shorts; live/shadow reversal threshold mismatch**
  - Source: `AUDIT_PORTFOLIO_LONG_SHORT_2026-03-31.md` / HIGH-5
  - Files: `config/live/portfolio.yaml:4`, `config/shadow/portfolio.yaml:4`, `core/execution/common/portfolio/portfolio_intent_engine.hpp:117, 145`
  - Problem: Only `negative_reversal_signal_bps` exists; there is no `positive_reversal_signal_bps` to urgently exit a short when signal flips strongly bullish. Short exits in shadow mode depend on the `positive_entry` path flipping the target sign (which is functionally correct but lacks an explicit reason code and urgency guarantee). Additionally, the live threshold (`–1.0`) and shadow threshold (`–3.0`) differ substantially, creating a train/prod mismatch.
  - Acceptance criteria:
    - [ ] `positive_reversal_signal_bps` config field added to `PortfolioIntentConfig` and both YAML files.
    - [ ] `PortfolioIntentEngine::evaluate` checks `signal_bps >= positive_reversal_signal_bps` when holding a short position and triggers `flatten_now` with urgency `AGGRESSIVE` and a new `POSITIVE_REVERSAL` reason code.
    - [ ] A new `PortfolioIntentReasonCode::POSITIVE_REVERSAL` enum value is added and handled in `reason_code_to_string`.
    - [ ] Live and shadow `negative_reversal_signal_bps` values are reconciled; any intentional divergence is documented with a comment in the YAML.
    - [ ] Unit tests cover `short → flatten` via positive reversal for both threshold-exactly-met and exceeded cases.

- [x] **PORT-H5: Wire futures risk gates (`commit_futures_order`) into live submit path**
  - Source: `AUDIT_PORTFOLIO_LONG_SHORT_2026-03-31.md` / HIGH-4 (also tracked as FUT-BN-15)
  - File: `core/engine/trading_engine_main.cpp:787–792`
  - Problem: In both spot and futures modes, order submission calls `global_risk.commit_order(...)`. The futures-specific `GlobalRiskControls::commit_futures_order(...)` — enforcing leverage caps, maintenance margin, and mark/index divergence — is never invoked. Futures risk constraints are fully implemented but unreachable.
  - Acceptance criteria:
    - [ ] In the `futures_only_mode` branch, `global_risk.commit_futures_order(...)` is called with a fully populated `FuturesRiskContext` (leverage, mark price, maintenance margin, funding rate from `BinanceFuturesConnector`).
    - [ ] Rejection reason codes for leverage, margin, mark/index divergence, and funding are logged.
    - [ ] Integration tests cover hard rejection and funding-aware notional scaling.
    - [ ] Generic `commit_order` is no longer called for futures submits.

---

## MEDIUM

- [x] **PORT-M1: `ALPHA_NEGATIVE` reason code appended before deadband zero-out**
  - Source: `AUDIT_PORTFOLIO_LONG_SHORT_2026-03-31.md` / MEDIUM-1
  - File: `core/execution/common/portfolio/portfolio_intent_engine.hpp:169–178`
  - Problem: The `ALPHA_NEGATIVE` (or `ALPHA_POSITIVE`) reason is appended before the deadband check clears the target to zero. A signal within the deadband that also exceeds the short-entry threshold emits `ALPHA_NEGATIVE` with zero position delta, misleading log and metric consumers.
  - Acceptance criteria:
    - [ ] Deadband check occurs before reason-code appending, or the reason code is overridden to `ALPHA_DECAY` when `|signal| <= deadband_signal_bps`.
    - [ ] Unit test verifies that a signal within deadband always yields `ALPHA_DECAY` reason code regardless of sign.

- [x] **PORT-M3: Three independent staleness thresholds with no unified configuration source**
  - Source: `AUDIT_PORTFOLIO_LONG_SHORT_2026-03-31.md` / MEDIUM-3
  - Files: `core/ipc/alpha_signal.hpp:23` (`STALE_NS = 2 s`), `config/live/portfolio.yaml:8` (`stale_signal_ms: 1500`), `core/execution/market_maker.hpp:42` (`cfg_.stale_ns`)
  - Problem: Three components each implement an independent staleness check with different values (2 000 ms, 1 500 ms, per-MM config). A signal aged 1 600 ms is stale to the intent engine but live to the routing gate, allowing an order to be routed on a signal the intent engine would have blocked.
  - Acceptance criteria:
    - [ ] `STALE_NS` compile-time constant in `AlphaSignalReader` is replaced by a constructor parameter sourced from the same `stale_signal_ms` config key used by `PortfolioIntentEngine`.
    - [ ] `MarketMakerConfig::stale_ns` is removed or aligned; the MM reads from the shared config value.
    - [ ] All three consumers use the same runtime-configurable value. One config key (`stale_signal_ms`) governs all.
    - [ ] Unit tests for `AlphaSignalReader::allows_long()` and the intent engine `STALE_SIGNAL` path use identical thresholds.

- [x] **PORT-M4: Reconciliation resets inventory age for pre-existing positions**
  - Source: `AUDIT_PORTFOLIO_LONG_SHORT_2026-03-31.md` / MEDIUM-4
  - File: `core/execution/common/portfolio/position_ledger.hpp:131–136`
  - Problem: On the first reconciliation after startup, any venue with a non-zero position that lacks `has_inventory_age` has `opened_at` set to `now()`, silently restarting the stale-inventory timer. A position held before a restart will not trigger a forced exit for `stale_inventory_ms` regardless of its true age.
  - Acceptance criteria:
    - [ ] If `ReconciliationSnapshot` carries position age (or open timestamp), `reconcile_positions` propagates it into `opened_at` rather than using `now()`.
    - [ ] If the snapshot does not carry age, a `LOG_WARN` is emitted noting that inventory age was reset for a non-zero position.
    - [ ] Unit test covers reconciliation of a pre-existing position: `has_inventory_age` is set and `opened_at` is not clamped to now when a valid age is available.

- [x] **PORT-M5: Use true futures mid price for basis guard in intent engine**
  - Source: `AUDIT_PORTFOLIO_LONG_SHORT_2026-03-31.md` / MEDIUM-5 (also tracked as FUT-BN-16)
  - File: `core/engine/trading_engine_main.cpp:930`
  - Problem: `intent_ctx.futures_mid_price` is set to `binance_book.mid_price()` (spot) in futures mode, making `compute_basis_bps` return 0.0 always. The `max_basis_divergence_bps: 25.0` guard is permanently inactive in futures mode.
  - Acceptance criteria:
    - [ ] `futures_mid_price` is sourced from `BinanceFuturesConnector` mark/contract mid when `futures_only_mode = true`.
    - [ ] `max_basis_divergence_bps` correctly triggers `BASIS_TOO_WIDE` flatten when basis exceeds threshold in futures mode.
    - [ ] Portfolio-intent log line includes both `spot_mid` and `futures_mid` values used in basis computation.
    - [ ] Unit/integration test verifies `BASIS_TOO_WIDE` fires deterministically in futures mode when injected basis exceeds threshold.

- [ ] **FUT-BN-8: Shadow-mode rollout plan for Binance futures**
  - Scope: shadow execution path for futures with no live order placement, plus metrics for slippage, reject rate, and reconciliation drift.
  - Acceptance criteria:
    - [ ] Shadow mode can run as a dedicated session with futures trading explicitly activated (`futures_enabled=true`) while final order-placement side effects remain disabled.
    - [ ] Shadow mode reuses production code paths except final submit side effects.
    - [ ] Daily report includes futures-specific health metrics and drift deltas.
    - [ ] Go-live checklist defines promotion criteria and rollback conditions.

- [ ] **FUT-BN-9: Observability and audit trail for futures lifecycle**
  - Scope: structured logs + metrics for futures request/response latencies, error buckets, and order lifecycle transitions.
  - Acceptance criteria:
    - [ ] Metrics include per-endpoint p50/p95/p99 latency and reject taxonomy.
    - [ ] Audit logs can correlate `client_order_id` to Binance futures order IDs and reconciliation events.
    - [ ] No secrets appear in logs.

---

## LOW

- [ ] **PORT-L1: `check_stop` in `NeuralAlphaMarketMaker` ignores its `AlphaSignal` parameter**
  - Source: `AUDIT_PORTFOLIO_LONG_SHORT_2026-03-31.md` / LOW-1
  - File: `core/execution/market_maker.hpp:193`
  - Problem: `void check_stop(double mid, const AlphaSignal& )` receives a signal but the parameter is unnamed and unused. Signal-informed stop widening (e.g., tighter stop when risk is elevated) is not implemented, and the unnamed parameter creates confusion about whether this was intentional.
  - Acceptance criteria:
    - [ ] Either name the parameter and document why it is intentionally unused (with a `(void)` suppress), or implement signal-aware stop adjustment as a separate TODO with a design note.
    - [ ] No functional behavior change in this ticket.

- [ ] **PORT-L2: `AlphaSignalReader::read()` does not auto-reconnect; `RegimeSignalReader::read()` does**
  - Source: `AUDIT_PORTFOLIO_LONG_SHORT_2026-03-31.md` / LOW-2
  - Files: `core/ipc/alpha_signal.hpp:67–69`, `core/ipc/regime_signal.hpp:62–65`
  - Problem: If `/tmp/trt_ipc/trt_alpha.bin` appears after engine startup, alpha signals are never picked up without a full restart. The regime reader auto-reconnects. This asymmetry is undocumented and surprising.
  - Acceptance criteria:
    - [ ] `AlphaSignalReader::read()` attempts `open()` if `ptr_ == nullptr` (mirroring regime reader behavior), OR the asymmetry is explicitly documented with a rationale comment in both headers.
    - [ ] Chosen behavior is consistent between both readers.

- [ ] **PORT-L3: `intent_action()` returns `"reduce"` for both reducing longs and reducing shorts**
  - Source: `AUDIT_PORTFOLIO_LONG_SHORT_2026-03-31.md` / LOW-3
  - File: `core/engine/trading_engine_main.cpp:224–232`
  - Problem: A negative `position_delta` maps to `"reduce"` whether the intent is closing a long, adding to a short, or reducing a short. Shadow-mode logs label a short-entry as `"reduce"`, breaking intent tracking in observability dashboards.
  - Acceptance criteria:
    - [ ] Action labels distinguish direction: `"reduce_long"` (selling from long) vs `"enter_short"` (initiating short from flat) vs `"reduce_short"` (buying back a short) vs `"enter_long"`.
    - [ ] Labels require `current_position` context to determine; `intent_action` signature is updated accordingly, or a new helper is introduced.
    - [ ] Shadow metrics and Grafana dashboards (if any) are updated to handle new label values.

- [ ] **FUT-BN-10: Multi-Assets Mode and portfolio-margin compatibility review**
  - Scope: document compatibility matrix for one-way vs hedge mode, single-asset vs multi-assets mode, and future portfolio-margin migration.
  - Acceptance criteria:
    - [ ] Supported combinations are explicitly documented with constraints.
    - [ ] Unsupported account modes fail with clear startup diagnostics.

---

## RESEARCH

- [ ] **FUT-R-1: Evaluate basis/term-structure alpha for hedge timing**
  - Scope: research signal utility from perp basis, funding regime, and spot-perp dislocation for inventory hedging.
  - Acceptance criteria:
    - [ ] Offline report quantifies incremental alpha and drawdown impact.
    - [ ] Candidate features and productionization risk are documented.

- [ ] **FUT-R-2: Research spot/futures coupling for hedging overlay (deferred)**
  - Scope: investigate later-phase coupling where spot and futures can be co-activated for hedge overlays (e.g., basis hedges, inventory neutralization).
  - Acceptance criteria:
    - [ ] Research note defines candidate coupling architectures, risk controls, and failure modes.
    - [ ] Backtest/replay quantifies whether hedge overlay improves drawdown/variance without degrading core alpha.
    - [ ] Output includes go/no-go criteria before any production implementation task is created.
