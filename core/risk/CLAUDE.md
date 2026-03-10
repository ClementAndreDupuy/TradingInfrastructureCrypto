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

- `evaluate_arb()`: < 500 ns on x86-64
- No heap allocation after construction
- Lock-free atomic operations only
