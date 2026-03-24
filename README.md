# TradingInfrastructureCrypto Architecture Overview

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

  subgraph Ops["Ops + Runtime Control"]
    CFG["config/dev|shadow|live\nRuntime parameters + credentials refs"]
    DEPLOY["deploy/\nsystemd + AWS deployment assets"]
    MON["Prometheus + Grafana\nDashboards + alerts"]
  end

  subgraph Shared["Shared + Integration Layer"]
    COMMON["core/common\nOrder, fills, enums, shared types"]
    IPCW["core/ipc\nPython shared-memory signal writer"]
    IPCR["core/ipc\nC++ AlphaSignalReader"]
    BIND["core/bindings/\npybind11 bridge"]
  end

  subgraph HotPath["C++ Hot Path (latency critical)"]
    FEEDS["core/feeds\nWebSocket handlers\n(snapshot + deltas + sequence checks)"]
    ORDERBOOK["core/orderbook\nLocal order books"]
    ENGINE["core/engine\nRuntime orchestration"]
    EXEC["core/execution\nOrderManager + MarketMaker"]
    SOR["core/execution\nSmartOrderRouter + ExchangeConnector(s)"]
    RISK["core/risk\nPre-trade checks\nKill switch / circuit breaker"]
    SHADOW["core/shadow\nPaper-trading engine"]
  end

  subgraph ColdPath["Python Cold Path (research + model)"]
    PIPE["research/neural_alpha\nFeature pipeline + training + runtime\n(direct core/ feature ingestion)"]
    REGIME["research/regime\nHMM regime probabilities + IPC publish"]
    BACKTEST["research/backtest\nShadow/execution metrics + backtests"]
    TESTS["tests/unit|integration|replay|perf\nRegression + performance checks"]
  end

  ENGINE -->|"Load params + risk limits"| CFG
  EXEC -->|"Load strategy + venue config"| CFG
  FEEDS -->|"Load symbols + feed config"| CFG
  DEPLOY -->|"Provision + service rollout"| ENGINE

  COMMON --> FEEDS
  COMMON --> ORDERBOOK
  COMMON --> RISK
  COMMON --> EXEC
  COMMON --> SHADOW
  COMMON --> ENGINE
  COMMON --> BIND

  FEEDS -->|"Normalized market data stream"| PIPE
  ORDERBOOK -->|"LOB state + derived microstructure features"| PIPE
  EXEC -->|"Execution telemetry + fills"| PIPE
  PIPE -->|"Backtest-ready features/signals"| BACKTEST
  PIPE -->|"Alpha signal stream"| IPCW
  REGIME -->|"Regime probability stream"| IPCW
  PIPE -->|"Regime training features"| REGIME
  BIND -->|"Research helpers + native accelerators"| PIPE
  IPCW -->|"Shared memory"| IPCR
  IPCR -->|"Signal read"| EXEC

  ENGINE -->|"Start/stop + wiring"| FEEDS
  ENGINE -->|"Start/stop + wiring"| EXEC
  ENGINE -->|"Start/stop + wiring"| SOR
  ENGINE -->|"Start/stop + wiring"| SHADOW

  FEEDS -->|"Live feed connector (production)"| BIN
  FEEDS -->|"Live feed connector (production)"| KRA
  FEEDS -->|"Live feed connector (production)"| OKX
  FEEDS -->|"Live feed connector (production)"| COI

  BIN -->|"Market data\n(snapshot + deltas)"| FEEDS
  KRA -->|"Market data\n(snapshot + deltas)"| FEEDS
  OKX -->|"Market data\n(snapshot + deltas)"| FEEDS
  COI -->|"Market data\n(snapshot + deltas)"| FEEDS

  FEEDS --> ORDERBOOK
  ORDERBOOK -->|"Top of book + depth"| EXEC
  EXEC -->|"Pre-trade check request"| RISK
  RISK -->|"Approve/reject"| EXEC
  EXEC -->|"Validated parent/child orders"| SOR

  SOR -->|"Live order connector (production)"| BIN
  SOR -->|"Live order connector (production)"| KRA
  SOR -->|"Live order connector (production)"| OKX
  SOR -->|"Live order connector (production)"| COI
  BIN -->|"Ack/reject/fill updates"| SOR
  KRA -->|"Ack/reject/fill updates"| SOR
  OKX -->|"Ack/reject/fill updates"| SOR
  COI -->|"Ack/reject/fill updates"| SOR
  SOR -->|"Normalized execution reports/fills"| EXEC
  EXEC -->|"Shadow route"| SHADOW
  SHADOW -->|"Paper fills"| EXEC

  FEEDS -->|"Feed/latency metrics"| MON
  EXEC -->|"Orders/PnL/risk metrics"| MON
  SOR -->|"Venue routing + reject metrics"| MON
  SHADOW -->|"Shadow performance"| MON
  PIPE -->|"Training + inference metrics"| MON
  ENGINE -->|"Service health"| MON

  TESTS -->|"CI validation of C++ + Python paths"| FEEDS
  TESTS -->|"CI validation of strategy/risk behavior"| EXEC
  TESTS -->|"Replay/perf checks"| BACKTEST
```

## How each block is used

## Recent updates

- ✅ **Four fully working venues for feeds and orders**: Binance, Kraken, OKX, and Coinbase are now represented as live production connectors on both the market data path (`core/feeds`) and execution path (`core/execution` smart order router).
- ✅ **Neural network now consumes data directly from `core/`**: `research/neural_alpha` receives normalized market data, order book state, and execution telemetry directly from core components for model training/inference inputs.

- **`core/feeds`**: Maintains real-time exchange market data streams and validates message order before updating internal state.
- **`core/orderbook`**: Stores the in-memory book per symbol; execution logic reads this for spread, depth, and microstructure signals.
- **`core/risk`**: Gatekeeper in front of trading actions; enforces limits and can halt trading fast.
- **`core/execution`**: Decides quotes/orders, tracks positions, and hands validated orders to the smart order router for venue submission.
- **`core/ipc`**: Shared-memory bridge between Python model output and C++ strategy input.
- **`core/execution` smart order router**: Splits/routes validated orders to Binance/Kraken/OKX/Coinbase connectors and normalizes venue acks/fills back to execution.
- **`core/shadow`**: Runs the same trading logic in paper mode to validate behavior before live deployment.
- **`research/neural_alpha`**: Trains and generates alpha signals from features ingested directly from `core/feeds`, `core/orderbook`, and execution telemetry.
- **`research/regime`**: Learns regime probabilities (`calm`, `trending`, `shock`, `illiquid`) and publishes them over IPC for live consumption.
- **`research/backtest`**: Produces shadow/execution metrics and replays market events to evaluate strategy quality offline.

## Typical lifecycle (short)

1. **Research** trains/tests a model in Python (`research/*`).
2. Model writes alpha signals to **IPC shared memory**.
3. C++ **execution** reads signal + order book state + risk checks.
4. Orders go either to **shadow** (paper) or **live exchanges**.
5. Metrics flow to monitoring for operational health and performance.

## Build/Bootstrap preflight

Run the dependency checker before local builds or production bootstrap:

```bash
python3 deploy/scripts/preflight_check.py
```

It validates required tooling and native dependencies (`cmake`, `pkg-config`, `libwebsockets`, `libcurl`, `nlohmann/json`) and exits non-zero on missing prerequisites.
