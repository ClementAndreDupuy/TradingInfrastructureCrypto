# core/risk/

Pre-trade risk checks. **Sub-microsecond requirement.**

## Critical Principle

**Risk layer MUST execute before order submission.** No exceptions.

Every order passes through:
```
Signal → Risk Check → Execution
         ↓
      REJECT if violated
```

## Pre-Trade Checks

### 1. Position Limits
```cpp
// Per-symbol position limits
if (abs(current_position + order_qty) > max_position[symbol]) {
    reject("Position limit exceeded");
}
```

### 2. Notional Limits
```cpp
// Per-symbol and portfolio-wide notional
double order_notional = order_qty * price;
if (current_notional + order_notional > max_notional) {
    reject("Notional limit exceeded");
}
```

### 3. Drawdown Breakers
```cpp
// Stop trading if drawdown exceeds threshold
double current_pnl = portfolio.unrealized_pnl();
if (current_pnl < -max_drawdown) {
    kill_switch_triggered = true;
    reject("Drawdown breaker triggered");
}
```

### 4. Rate Limits
```cpp
// Order rate per symbol and per exchange
if (orders_sent_last_second[symbol] >= max_orders_per_second) {
    reject("Rate limit exceeded");
}
```

### 5. Market Hours
```cpp
// Don't trade outside market hours or during halts
if (!is_trading_hours(symbol) || is_halted(symbol)) {
    reject("Market closed or halted");
}
```

## Kill Switch

**Must work even if application is broken.** Implement at multiple levels:

### Software Kill Switch
```cpp
// Global atomic flag, checked before every order
std::atomic<bool> kill_switch_active{false};

if (kill_switch_active.load(std::memory_order_acquire)) {
    reject("Kill switch active");
    return;
}
```

### Hardware Kill Switch
- Physical button/switch that cuts network or power
- Independent of software state
- Test regularly (weekly)

### Dead Man's Switch
- Requires periodic heartbeat
- If heartbeat stops, cancel all orders and flatten positions
- Protects against process crash

## Performance Requirements

- **Check latency**: < 500 nanoseconds
- **No heap allocation** - All limits pre-configured
- **Lock-free** - Use atomic operations, no mutexes in hot path

## Configuration

Risk limits in `config/{dev,shadow,live}/risk.yaml`:
```yaml
position_limits:
  BTC-USD: 10.0
  ETH-USD: 100.0

notional_limits:
  per_symbol: 1000000
  portfolio: 5000000

drawdown_breaker: -5000  # $5k max drawdown

rate_limits:
  orders_per_second: 10
  orders_per_minute: 100
```

## Testing

1. **Unit tests** - Each check in isolation
2. **Integration tests** - Full risk layer with mock orders
3. **Stress tests** - High order rate, verify no false positives
4. **Kill switch drill** - Weekly test of all kill mechanisms

## Common Mistakes

- **Checking after execution** - Risk check MUST be before, not after
- **Using Python** - Risk layer must be pure C++, no Python calls
- **Slow lookups** - Use hash maps, not linear search for limits
- **Missing edge cases** - What if position is exactly at limit? Define inclusive/exclusive
