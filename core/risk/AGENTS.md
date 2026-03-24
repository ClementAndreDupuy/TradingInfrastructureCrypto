# core/risk/

Pre-trade risk checks. **Sub-microsecond, no Python, no exceptions.**

## Checks (in order)

1. Kill switch (single atomic load)
2. Circuit breaker
3. Drawdown limit → triggers kill switch
4. Input validation
5. Book freshness (stale data = execution risk)
6. Flash crash guard (price deviation from reference)
7. Gross spread minimum
8. Fee-adjusted profitability
9. Open leg count
10. Rate limits (per-second and per-minute, lock-free)
11. Position limits (per symbol, per exchange)
12. Notional limits (per symbol, per exchange, portfolio)

## Kill Switch

Three levels: software (atomic flag), dead man's switch (heartbeat timeout), hardware (physical network cut). Test weekly.

## Configuration

Risk limits in `config/{dev,shadow,live}/risk.yaml`. Always tighter in live than shadow, tighter in shadow than dev.

## Performance

- Risk evaluation: < 500 ns on x86-64
- No heap allocation after construction
- Lock-free atomic operations only

- Keep comments only when they explain fail-fast ordering, atomic/concurrency rationale, or guard semantics that are not obvious from the code.
## Reusable Agent Memory (Updated)

- Preserve check ordering unless there is a strong latency/correctness reason; ordering encodes fail-fast policy.
- Any new guard should declare:
  - unit (ticks, bps, notional)
  - reset semantics
  - interaction with kill switch state
- Keep config parity across `config/dev`, `config/shadow`, `config/live` with stricter production limits.


## Classes & Methods (Quick Reference)

- **`CircuitBreaker` (`circuit_breaker.hpp`)** — Fast guardrail engine that triggers kill-switch on critical conditions.
  - `check_order_rate()/check_message_rate()`: Enforces per-second/per-minute order/message limits.
  - `check_drawdown()/check_book_age()/check_price_deviation()`: Detects drawdown, stale books, and flash-crash moves.
  - `record_leg_result()/check_consecutive_losses()`: Maintains consecutive-loss state and enforces loss streak limits.

- **`GlobalRiskControls` (`global_risk_controls.hpp`)** — Portfolio-level notional and concentration controls.
  - `check_order(...)`: Evaluates whether a proposed signed notional would breach limits.
  - `commit_order(...)`: Atomically commits exposures and triggers kill-switch on breaches.
  - `gross_notional()/net_notional()`: Returns current aggregate risk exposures.

- **`RecoveryGuard` (`recovery_guard.hpp`)** — Recovery-anomaly limiter.
  - `check(in_flight_ops, duplicate_acks, cancel_replace_races)`: Trips kill-switch if recovery counters exceed limits.

- **`RiskConfigLoader` (`risk_config_loader.hpp`)** — Runtime risk config parser.
  - `load(path, out)`: Reads key-value config and maps aliases into `RiskRuntimeConfig`.
