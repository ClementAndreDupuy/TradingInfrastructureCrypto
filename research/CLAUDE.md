# research/

**Python cold path** - No latency constraint. Research, backtesting, monitoring.

## Critical Rules

1. **Never call from C++ hot path** - This layer is async, accessed via IPC
2. **Use Polars, not Pandas** - Polars is 10-100x faster for tick data
3. **Walk-forward validation** - Crypto regimes change frequently, backtest with rolling windows
4. **Store everything** - Disk is cheap, missing data is expensive

## Components

- **data/** - Storage connectors (Arctic, kdb+, Parquet)
- **features/** - Signal/feature engineering
- **alpha/** - Alpha models, IC/ICIR analysis
- **backtest/** - Event-driven backtest engine
- **notebooks/** - Exploratory analysis (never production logic)

## Technology Stack

- **Polars** - DataFrame operations on tick data
- **NumPy** - Numerical computations
- **statsmodels** - Statistical analysis
- **Arctic / kdb+** - Time-series storage
- **pybind11** - Interface to C++ matching engine

## Data Schema

Every tick record must have:
```python
{
    'symbol': str,
    'exchange': str,
    'timestamp_exchange': int64,  # nanoseconds
    'timestamp_local': int64,     # nanoseconds
    'sequence': int64,
    'bid_prices': List[float],
    'bid_sizes': List[float],
    'ask_prices': List[float],
    'ask_sizes': List[float],
}
```

## Typical Research Workflow

```python
import polars as pl

# 1. Load tick data
df = pl.read_parquet('data/BTC-USD/2024-01.parquet')

# 2. Engineer features
df = df.with_columns([
    # Order flow imbalance
    ((pl.col('bid_sizes').list.sum() - pl.col('ask_sizes').list.sum()) /
     (pl.col('bid_sizes').list.sum() + pl.col('ask_sizes').list.sum())).alias('ofi'),

    # Microprice
    ((pl.col('bid_prices').list.first() * pl.col('ask_sizes').list.first() +
      pl.col('ask_prices').list.first() * pl.col('bid_sizes').list.first()) /
     (pl.col('bid_sizes').list.first() + pl.col('ask_sizes').list.first())).alias('microprice'),
])

# 3. Forward returns
df = df.with_columns(
    pl.col('microprice').shift(-100).alias('future_price')
).with_columns(
    ((pl.col('future_price') - pl.col('microprice')) / pl.col('microprice')).alias('forward_return')
)

# 4. IC analysis
ic = df.select([
    pl.corr('ofi', 'forward_return').alias('IC')
])

print(f"Information Coefficient: {ic['IC'][0]:.4f}")
```

## Backtesting

See `backtest/CLAUDE.md` for detailed guidance.

**Key principle:** Event-driven, NOT vectorized. Must model:
- Latency budget
- Partial fills
- Queue position
- Fees
- Market impact

## Common Pitfalls

- **Using Pandas** - Too slow for tick data (hundreds of millions of rows)
- **Vectorized backtest** - Unrealistic fill assumptions, use event-driven
- **In-sample overfitting** - Always use walk-forward validation
- **Ignoring latency** - Signal at time T, execution at T+latency
- **Mid-price backtesting** - Must use actual bid/ask, not mid
