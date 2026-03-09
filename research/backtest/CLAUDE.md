# research/backtest/

Event-driven backtest engine with C++ matching core.

## Architecture

```
Python Strategy Layer
        ↓
  Signal Generation
        ↓
C++ Matching Engine (via pybind11)
        ↓
  Fill Simulation
        ↓
  PnL Calculation
```

## Critical Requirements

### 1. Event-Driven (NOT Vectorized)

```python
# WRONG: Vectorized
signals = df['feature'] > threshold
returns = df['return'] * signals.shift(1)
pnl = returns.sum()

# RIGHT: Event-driven
for tick in orderbook_replay:
    signal = strategy.on_tick(tick)
    if signal:
        order = create_order(signal)
        fills = matching_engine.submit(order, tick.book)
        pnl += calculate_pnl(fills)
```

### 2. Realistic Latency Modeling

```python
# Signal generated at time T
signal_time = tick.timestamp

# Order arrives at exchange at T + latency
execution_time = signal_time + latency_budget_ns

# Book state at execution time (NOT signal time)
future_book = get_book_at_time(execution_time)
fills = matching_engine.submit(order, future_book)
```

### 3. Partial Fill Simulation

```python
# Order for 1.0 BTC, but only 0.3 BTC at best bid
order = LimitOrder(side=BUY, price=50000, qty=1.0)

# Matching engine returns partial fill
fills = [
    Fill(qty=0.3, price=50000, timestamp=T),
    Fill(qty=0.5, price=49999, timestamp=T+100ms),  # Resting order filled later
    # 0.2 BTC remains unfilled
]
```

### 4. Queue Position Modeling

For limit orders, track queue position:
```python
# Join queue at price level
queue_position = book.get_queue_size(price, side)

# On each trade at this level, move forward in queue
if trade.price == our_price:
    queue_position -= trade.size
    if queue_position <= 0:
        # We got filled
        fill_qty = min(-queue_position, remaining_qty)
```

### 5. Fee Modeling

```python
# Maker/taker fee asymmetry
if order.type == LIMIT and fills[0].liquidity == 'maker':
    fee = fills[0].qty * fills[0].price * maker_fee_rate
else:
    fee = fills[0].qty * fills[0].price * taker_fee_rate

pnl -= fee
```

## C++ Matching Engine Interface

Exposed to Python via pybind11:

```cpp
// bindings/matching_engine.cpp
class MatchingEngine {
public:
    std::vector<Fill> submit_order(
        const Order& order,
        const OrderBook& book,
        int64_t latency_ns
    );

    double get_market_impact(
        double quantity,
        const OrderBook& book
    );
};

PYBIND11_MODULE(matching, m) {
    py::class_<MatchingEngine>(m, "MatchingEngine")
        .def(py::init<>())
        .def("submit_order", &MatchingEngine::submit_order)
        .def("get_market_impact", &MatchingEngine::get_market_impact);
}
```

Python usage:
```python
from matching import MatchingEngine

engine = MatchingEngine()
fills = engine.submit_order(order, book, latency_ns=1000)
```

## Backtest Output

Every backtest must produce:
- **Trade log** - Every order, fill, cancel with timestamp and book state
- **PnL curve** - Cumulative PnL over time
- **Fill rate** - Percentage of orders filled
- **Slippage** - Difference between signal price and fill price
- **Sharpe ratio** - Risk-adjusted return
- **Max drawdown** - Largest peak-to-trough decline

## Walk-Forward Validation

```python
# Train on 3 months, test on 1 month, roll forward
train_periods = [
    ('2024-01', '2024-03'),  # Train
    ('2024-02', '2024-04'),  # Train
    ('2024-03', '2024-05'),  # Train
]

test_periods = [
    '2024-04',  # Test
    '2024-05',  # Test
    '2024-06',  # Test
]

for (train_start, train_end), test_month in zip(train_periods, test_periods):
    # Optimize on train
    params = optimize_strategy(train_start, train_end)

    # Test on out-of-sample
    pnl = backtest_strategy(params, test_month)
    print(f"{test_month}: {pnl:.2f}")
```

## Common Mistakes

1. **Point-in-time violation** - Using future data (e.g., close price to generate signal at open)
2. **Ignoring latency** - Assuming instant execution
3. **Perfect fills** - Assuming limit orders always fill at desired price
4. **No market impact** - Large orders don't move the market
5. **Mid-price execution** - Backtesting at mid instead of realistic bid/ask
6. **Survivorship bias** - Only testing on currently-traded symbols
7. **Over-optimization** - Fitting to noise in training period

## Performance

Use Polars for tick data processing:
```python
# Load 100M ticks
df = pl.scan_parquet('data/*.parquet').collect()

# Fast feature engineering
features = df.with_columns([
    pl.col('bid_size').rolling_sum(window_size=100).alias('bid_flow'),
    # ... more features
])

# C++ matching engine handles order simulation (fast)
```

Target: Backtest 1 month of tick data in < 10 minutes
