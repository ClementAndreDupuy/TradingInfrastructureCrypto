# ThamesRiverTrading Architecture Overview

This is a high-level map of how each system block talks to the others.

## Component Interaction Diagram

```mermaid
flowchart LR
  subgraph Exchanges["External Exchanges"]
    BIN["Binance"]
    KRA["Kraken"]
    OKX["OKX"]
    COI["Coinbase"]
  end

  subgraph HotPath["C++ Hot Path (latency critical)"]
    FEEDS["core/feeds\nWebSocket handlers\n(snapshot + deltas + sequence checks)"]
    ORDERBOOK["core/orderbook\nLocal order books"]
    EXEC["core/execution\nOrderManager + MarketMaker"]
    RISK["core/risk\nPre-trade checks\nKill switch / circuit breaker"]
    SHADOW["core/shadow\nPaper-trading engine"]
    IPCR["core/ipc\nAlphaSignalReader"]
  end

  subgraph ColdPath["Python Cold Path (research + model)"]
    PIPE["research/alpha/neural_alpha\nFeature pipeline + training"]
    BACKTEST["research/backtest\nEvent-driven backtests"]
    IPCW["core/ipc\nShared-memory signal writer"]
    MON["Monitoring\nPrometheus/Grafana"]
  end

  FEEDS -->|"WebSocket subscribe/auth"| BIN
  FEEDS -->|"WebSocket subscribe/auth"| KRA
  FEEDS -->|"WebSocket subscribe/auth"| OKX
  FEEDS -->|"WebSocket subscribe/auth"| COI

  BIN -->|"WebSocket market data\n(snapshot + deltas)"| FEEDS
  KRA -->|"WebSocket market data\n(snapshot + deltas)"| FEEDS
  OKX -->|"WebSocket market data\n(snapshot + deltas)"| FEEDS
  COI -->|"WebSocket market data\n(snapshot + deltas)"| FEEDS

  FEEDS --> ORDERBOOK
  ORDERBOOK --> EXEC
  EXEC --> RISK
  RISK --> EXEC

  EXEC -->|"REST/WebSocket orders"| Exchanges
  Exchanges -->|"Execution reports/fills"| EXEC
  EXEC -->|"Shadow route"| SHADOW
  SHADOW -->|"Paper fills"| EXEC

  PIPE --> BACKTEST
  PIPE -->|"Alpha signal"| IPCW
  IPCW -->|"Shared memory"| IPCR
  IPCR -->|"Signal read"| EXEC

  FEEDS -->|"Feed/latency metrics"| MON
  EXEC -->|"Orders/PnL/risk metrics"| MON
  SHADOW -->|"Shadow performance"| MON
  PIPE -->|"Training + inference metrics"| MON
```

## How each block is used

- **`core/feeds`**: Maintains real-time exchange market data streams and validates message order before updating internal state.
- **`core/orderbook`**: Stores the in-memory book per symbol; execution logic reads this for spread, depth, and microstructure signals.
- **`core/risk`**: Gatekeeper in front of trading actions; enforces limits and can halt trading fast.
- **`core/execution`**: Decides quotes/orders, tracks positions, sends live orders through exchange connectors.
- **`core/ipc`**: Shared-memory bridge between Python model output and C++ strategy input.
- **`core/shadow`**: Runs the same trading logic in paper mode to validate behavior before live deployment.
- **`research/alpha/neural_alpha`**: Trains and generates alpha signals from historical/live features.
- **`research/backtest`**: Replays market events to evaluate strategy quality offline.

## Typical lifecycle (short)

1. **Research** trains/tests a model in Python (`research/*`).
2. Model writes alpha signals to **IPC shared memory**.
3. C++ **execution** reads signal + order book state + risk checks.
4. Orders go either to **shadow** (paper) or **live exchanges**.
5. Metrics flow to monitoring for operational health and performance.

## Build/Bootstrap preflight

Run the dependency checker before local builds or production bootstrap:

```bash
python3 scripts/preflight_check.py
```

It validates required tooling and native dependencies (`cmake`, `pkg-config`, `libwebsockets`, `libcurl`, `nlohmann/json`) and exits non-zero on missing prerequisites.
