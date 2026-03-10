# research/backtest/

Event-driven backtest engine. C++ matching core exposed via pybind11.

## Architecture

Python strategy layer → signal generation → C++ matching engine (pybind11) → fill simulation → PnL calculation.

## C++ Bridge

Built from `bindings/`. Run `python3 bindings/setup.py build_ext --inplace` to compile. Use `--engine cpp` flag in `fetch_and_run.py` to activate. Falls back to pure Python if not built.

## Critical Requirements

1. **Event-driven, not vectorized** — process one tick at a time.
2. **Realistic latency** — order arrives at T+latency, book state at execution time.
3. **Partial fills** — model queue position and resting order dynamics.
4. **Fee asymmetry** — maker vs taker, per exchange.
5. **Never backtest at mid-price** — replay actual bid/ask.

## Required Backtest Outputs

Trade log, equity curve, fill rate, slippage, Sharpe ratio, max drawdown.

## Walk-Forward Validation

Train 3 months → test 1 month → roll forward. Never evaluate in-sample.

## Common Mistakes

1. Point-in-time violation (using future data)
2. Assuming instant execution
3. Assuming perfect fills
4. Ignoring market impact
5. Survivorship bias
6. Over-optimization on training period
