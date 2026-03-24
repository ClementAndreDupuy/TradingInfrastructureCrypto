# core/risk/

Runtime risk controls used by strategy and execution flows.

## Classes & Methods (Quick Reference)

- **`CircuitBreaker` (`circuit_breaker.hpp`)** — Guardrails for runtime throughput and market safety conditions.
  - `check_order_rate()/check_message_rate()`: Enforces order/message throughput limits.
  - `check_drawdown()/check_book_age()/check_price_deviation()`: Monitors drawdown, staleness, and large price shocks.
  - `record_leg_result()/check_consecutive_losses()`: Tracks loss streaks and breach status.

- **`GlobalRiskControls` (`global_risk_controls.hpp`)** — Notional and concentration cap checks.
  - `check_order(...)`: Simulates post-order risk state and returns breach code if any.
  - `commit_order(...)`: Commits exposure updates following a passing check.
  - `gross_notional()/net_notional()`: Read current aggregate exposures.

- **`RecoveryGuard` (`recovery_guard.hpp`)** — Recovery anomaly boundary checker.
  - `check(...)`: Validates in-flight and duplicate/race counters against configured caps.

- **`RiskConfigLoader` (`risk_config_loader.hpp`)** — Runtime config loader.
  - `load(path, out)`: Parses file values into `RiskRuntimeConfig`.
