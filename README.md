# ThamesRiverTrading

# Crypto Trading System — Architecture & Roadmap

## Overview

A full-stack, microsecond-latency trading system targeting Binance, OKX, and Coinbase.  
Stack: **C++ hot path** (order book, execution, risk) + **Python cold path** (research, backtest, monitoring).

-----

## System Modules

### 1. Order Book Centralizer (C++)

The most latency-critical component. Everything downstream depends on book integrity.

- Per-exchange WebSocket feed handlers (Binance, OKX, Coinbase each have distinct snapshot/delta protocols)
- Normalized L2/L3 book in memory — flat array keyed by price tick grid (avoid `std::map`, cache-unfriendly)
- Sequence number validation and gap detection — missed delta triggers re-snapshot
- Publishes consolidated book state via **shared memory** (LMAX Disruptor pattern) or **kernel-bypass networking** (DPDK/RDMA) if co-located
- Timestamping via **PTP/IEEE 1588** — not system clock

**Key risk:** Silent book divergence. A single delta application bug poisons every downstream decision.

-----

### 2. Data Collection (C++ capture + Python storage)

- C++ feed handler writes raw ticks to a **memory-mapped ring buffer**
- Separate consumer drains buffer → writes to **Arctic** or **kdb+** for time-series storage
- Every record tagged with: exchange timestamp, local receipt timestamp, sequence number
- Store both raw feed messages and normalized snapshots

-----

### 3. Alpha Seeker (Python)

Operates entirely on stored/replayed data — no latency constraint.

- Typical order book signals: order flow imbalance, trade-to-quote ratio, depth asymmetry, microprice vs mid
- Use **Polars** (not Pandas) for tick data at scale (hundreds of millions of rows)
- Research loop: hypothesis → feature engineering → IC/ICIR analysis → forward return correlation → candidate signal
- Walk-forward validation mandatory — crypto has frequent regime changes

-----

### 4. Backtest Engine (Python + C++ matching core)

- **Event-driven**, not vectorized — realistic fill modeling requires it
- Order book replay from stored data feeding a simulated matching engine
- Must model:
  - Latency budget (book state N microseconds after signal)
  - Partial fills and queue position for limit orders
  - Maker/taker fee asymmetry
  - Market impact
- Matching engine core in C++, exposed to Python via **pybind11**
- **Common mistake:** backtesting at mid-price. Must replay actual book.

-----

### 5. Shadow Trading Engine (C++ + Python monitoring)

- Full live stack, **paper orders only** — no real execution
- Validates: signal-to-order latency, fill rate assumptions, slippage vs backtest, risk trigger accuracy
- **Critical:** must be identical code path to live. Different execution path = not a real test.
- Every shadow order logged with full book state at decision time

Run shadow for **weeks, not days** before going live.

-----

### 6. Live Trading Engine (C++)

- **Signal consumer:** reads alpha signals from shared memory
- **Risk layer:** pre-trade checks (position limits, drawdown breakers, notional caps) — sub-microsecond, no Python
- **Order manager:** tracks open orders, partial fills, cancels
- **Execution:** direct WebSocket (Binance/OKX) or FIX (Coinbase)
- **Kill switch:** OS/hardware level — a software bug must not prevent stopping

-----

## Technology Stack

|Layer           |Language    |Tools / Libraries         |
|----------------|------------|--------------------------|
|Feed handlers   |C++         |Boost.Asio, libwebsockets |
|Order book      |C++         |Custom flat array         |
|IPC             |C++         |Shared memory, ZeroMQ     |
|Storage         |Python      |Arctic, kdb+, Parquet     |
|Alpha research  |Python      |Polars, NumPy, statsmodels|
|Backtest        |Python + C++|Custom engine, pybind11   |
|Risk / Execution|C++         |Custom                    |
|Monitoring      |Python      |Prometheus + Grafana      |

-----

## Build Order

|Phase    |Module                      |Est. Duration                                        |
|---------|----------------------------|-----------------------------------------------------|
|1        |Feed handler + Order book   |6–10 weeks                                           |
|2        |Data collection + storage   |2–3 weeks                                            |
|3        |Backtest engine             |6–10 weeks                                           |
|4        |Alpha seeker (first signals)|4–8 weeks                                            |
|5        |Shadow trading engine       |4–6 weeks                                            |
|6        |Live trading engine         |4–8 weeks                                            |
|**Total**|                            |**~6–9 months** (solo), **~4–6 months** (2 engineers)|

These are working estimates assuming production-quality code with proper testing. Cutting corners on phases 1 or 6 is where real money gets lost.

-----

## Timeline Risks

- **Exchange quirks:** Binance and OKX have meaningfully different book update mechanics. A unified adapter is not trivial — budget extra time.
- **Latency validation:** Achieving true microsecond performance requires profiling, cache optimization, and potentially kernel bypass. This isn’t a one-pass effort.
- **Shadow phase:** Do not skip or shorten. It is the only real validation before capital is at risk.
- **Alpha decay:** The longer phases 1–3 take, the less historical relevance the backtest period has. Prioritize speed on infrastructure without compromising correctness.

-----

## Key Design Principles

1. **C++ owns the hot path.** Python never touches live execution or risk.
1. **Compress time on research. Never on risk controls.**
1. **Shadow = live code path.** Any divergence invalidates the test.
1. **Book integrity is foundational.** Every other module inherits its correctness from the order book.
1. **Kill switch is non-negotiable.** It must work even when the application is broken.
