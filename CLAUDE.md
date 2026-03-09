# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## System Overview

A microsecond-latency crypto trading system targeting Binance, OKX, and Coinbase.

**Critical architecture split:**
- **C++ hot path**: Order book, execution, risk (latency-critical)
- **Python cold path**: Research, backtesting, monitoring (no latency constraint)

## Build Commands

### C++ Components

**Quick Build (No CMake Required):**
```bash
# Build all components
./scripts/build.sh

# Run Binance feed example
./scripts/run_binance_example.sh
```

**With CMake:**
```bash
# Build C++ modules (order book, feeds, execution)
mkdir -p build && cd build
cmake ..
make -j$(nproc)

# Run C++ unit tests
make test
# Or with GTest directly
./tests/unit/orderbook_test
```

### Python Components
```bash
# Install Python dependencies
pip install -e .

# Run Python tests
pytest tests/unit/
pytest tests/integration/

# Run backtest
python -m research.backtest.engine --config config/dev/backtest.yaml
```

## Key Architecture Principles

1. **C++ owns the hot path** - Python NEVER touches live execution or risk
2. **Order book integrity is foundational** - All downstream modules depend on book correctness
3. **Shadow = live code path** - Shadow trading must use identical code to validate before live deployment
4. **Kill switch is non-negotiable** - Must work even when application is broken (OS/hardware level)

## Critical Components

### Order Book Centralizer (`core/orderbook/`)
The most latency-critical component. Uses flat array keyed by price tick grid (not `std::map`).
- Per-exchange WebSocket handlers in `core/feeds/{binance,okx,coinbase}/`
- Sequence number validation with gap detection
- Publishes via shared memory (LMAX Disruptor pattern)
- Timestamping via PTP/IEEE 1588, NOT system clock

**Key risk:** Silent book divergence poisons all downstream decisions.

### Data Collection
- C++ feed handler → memory-mapped ring buffer
- Consumer drains to Arctic/kdb+/Parquet
- Every record tagged with: exchange timestamp, local receipt timestamp, sequence number

### Backtest Engine (`research/backtest/`)
- **Event-driven**, NOT vectorized
- C++ matching core exposed to Python via pybind11 (`bindings/`)
- Must model: latency budget, partial fills, queue position, fees, market impact
- **Never backtest at mid-price** - replay actual book

### Shadow Trading Engine (`core/shadow/`)
Paper orders only, but MUST use identical code path to live trading.
- Run for weeks before going live
- Logs every decision with full book state

### Live Trading Engine (`core/execution/`, `core/risk/`)
- Risk layer: pre-trade checks, sub-microsecond, no Python
- Direct WebSocket (Binance/OKX) or FIX (Coinbase)

## Technology Stack

**C++:** Boost.Asio, libwebsockets, custom flat arrays, ZeroMQ, pybind11
**Python:** Polars (NOT Pandas for tick data), NumPy, statsmodels, pytest
**Storage:** Arctic, kdb+, Parquet
**Monitoring:** Prometheus, Grafana

## Directory Structure

```
core/              # C++ hot path components
├── orderbook/     # Book structure and delta application
├── feeds/         # Exchange-specific WebSocket handlers
├── ipc/           # Shared memory, ring buffers
├── risk/          # Pre-trade checks, kill switch
├── execution/     # Order manager, exchange connectors
└── shadow/        # Shadow trading engine

research/          # Python cold path
├── data/          # Storage connectors
├── features/      # Signal engineering
├── alpha/         # Alpha models
├── backtest/      # Backtest engine
└── notebooks/     # Exploratory only (never production)

bindings/          # pybind11 C++↔Python bridge
config/            # Environment-specific configs (dev/shadow/live)
tests/
├── unit/          # Per-module tests
├── integration/   # Cross-module tests
└── replay/        # Full stack regression with stored data
```

## Development Guidelines

### Code Style & Standards

#### C++ (Hot Path)
```cpp
// File naming: snake_case.hpp / snake_case.cpp
// Class naming: PascalCase
// Function/variable naming: snake_case
// Constants: UPPER_SNAKE_CASE

class OrderBook {
public:
    // Public interface first
    void apply_delta(const Delta& delta);

private:
    // Private members with trailing underscore
    std::vector<PriceLevel> bids_;
    std::vector<PriceLevel> asks_;
    uint64_t sequence_;
};

// Use const everywhere possible
void process_message(const Message& msg) {
    const auto& data = msg.data();  // const reference
}

// Use std::array or std::vector, NEVER raw pointers in hot path
std::array<PriceLevel, MAX_LEVELS> levels_;  // Good
PriceLevel* levels_;                          // Avoid
```

**C++ Standards:**
- Use C++17 or later
- No exceptions in hot path (use return codes or assertions)
- No RTTI (`dynamic_cast`, `typeid`) in hot path
- Prefer `std::array` over `std::vector` when size is known
- Use `constexpr` for compile-time computation
- Include guards: `#pragma once`

#### Python (Cold Path)
```python
# File naming: snake_case.py
# Class naming: PascalCase
# Function/variable naming: snake_case
# Constants: UPPER_SNAKE_CASE

import polars as pl
import numpy as np

class AlphaModel:
    """Docstrings for all classes and public methods."""

    def calculate_signal(self, df: pl.DataFrame) -> pl.DataFrame:
        """
        Calculate trading signal from features.

        Args:
            df: DataFrame with features

        Returns:
            DataFrame with 'signal' column
        """
        return df.with_columns([
            pl.col('feature').alias('signal')
        ])

# Type hints everywhere
def compute_ic(predictions: np.ndarray, returns: np.ndarray) -> float:
    return np.corrcoef(predictions, returns)[0, 1]
```

**Python Standards:**
- Python 3.10+
- Type hints required for all function signatures
- Use Polars for DataFrames, NOT Pandas (except for compatibility)
- Use NumPy for numerical computation
- Format with `black` and lint with `ruff`
- Docstrings in Google style

### Performance Requirements

#### Latency Budgets
```
Order book delta application:     < 1 microsecond
Risk check:                        < 1 microsecond
Feed handler processing:           < 10 microseconds
Signal generation (Python):        No constraint
Backtest (1 month tick data):      < 10 minutes
```

#### Memory Constraints
```
Order book per symbol:             < 100 MB
Total process memory (C++):        < 4 GB
Ring buffer size:                  Configurable, default 1 GB
```

#### Performance Validation
```bash
# Profile before committing hot path changes
perf record -g ./build/core/orderbook_bench
perf report

# Verify no heap allocations in hot path
valgrind --tool=massif ./build/core/orderbook_test
```

### Memory Management

#### C++ Hot Path Rules
1. **Pre-allocate everything at startup**
   ```cpp
   // Initialization
   void OrderBook::initialize(size_t max_levels) {
       bids_.reserve(max_levels);    // Pre-allocate
       asks_.reserve(max_levels);
   }

   // Hot path - no allocations
   void OrderBook::apply_delta(const Delta& delta) {
       // Only use pre-allocated memory
   }
   ```

2. **Use object pools for frequent allocations**
   ```cpp
   ObjectPool<Order> order_pool(1000);
   auto* order = order_pool.acquire();  // Reuse, don't allocate
   ```

3. **Avoid dynamic polymorphism in hot path**
   ```cpp
   // Bad: Virtual function call overhead
   virtual void process() = 0;

   // Good: Static polymorphism with templates
   template<typename Handler>
   void process(Handler& handler) {
       handler.on_message();
   }
   ```

### Error Handling & Logging

#### C++ Error Handling
```cpp
// Hot path: Use return codes, NOT exceptions
enum class Result { SUCCESS, ERROR_INVALID_SEQUENCE, ERROR_INVALID_PRICE };

Result OrderBook::apply_delta(const Delta& delta) {
    if (delta.sequence != expected_sequence_) {
        return Result::ERROR_INVALID_SEQUENCE;
    }
    // ... apply delta
    return Result::SUCCESS;
}

// Cold path: Exceptions are OK
void load_config(const std::string& path) {
    if (!file_exists(path)) {
        throw std::runtime_error("Config file not found");
    }
}
```

#### Logging Levels
```cpp
// Use structured logging
LOG_DEBUG("Book updated",
    "symbol", symbol,
    "sequence", sequence,
    "latency_ns", latency);

LOG_ERROR("Sequence gap detected",
    "expected", expected,
    "received", received,
    "symbol", symbol);

// Never log in hot path inner loops
// Aggregate and log periodically instead
```

**Log Levels:**
- `DEBUG`: Development only, disabled in production
- `INFO`: Important state changes (connection, snapshot)
- `WARN`: Recoverable errors (reconnect, re-snapshot)
- `ERROR`: Unrecoverable errors (kill switch triggered)

### Security Guidelines

#### API Keys & Credentials
```python
# NEVER hardcode credentials
# Bad
api_key = "sk_live_abc123"

# Good - use environment variables
import os
api_key = os.environ.get("BINANCE_API_KEY")

# Better - use secrets management
from config import load_secret
api_key = load_secret("binance_api_key")
```

**Rules:**
- Store credentials in `config/live/secrets.yaml` (gitignored)
- Use separate API keys for dev/shadow/live
- Rotate keys quarterly
- Restrict API key permissions (read-only for research, trade for live)

#### Data Protection
- Never log API keys, even in debug mode
- Never commit `config/live/` directory
- Encrypt sensitive data at rest (kdb+ encryption, encrypted Parquet)

### Git Workflow

#### Branch Naming
```
feature/orderbook-optimization
bugfix/binance-sequence-gap
hotfix/kill-switch-trigger
research/new-alpha-signal
```

#### Commit Messages
```
feat: Add OKX feed handler with checksum validation

- Implement WebSocket connection with reconnect logic
- Add CRC32 checksum validation per OKX spec
- Include integration test with recorded data

Refs: #123
```

**Format:**
- `feat:` New feature
- `fix:` Bug fix
- `perf:` Performance improvement
- `refactor:` Code refactoring
- `test:` Test additions/changes
- `docs:` Documentation updates

#### Pull Request Process
1. Create feature branch from `main`
2. Write code and tests
3. Run full test suite: `make test && pytest`
4. Run linters: `clang-format` (C++) and `black` (Python)
5. Open PR with description of changes
6. Require 1 approval before merge
7. Squash commits when merging

### Documentation Requirements

#### When to Update CLAUDE.md Files
- **Always update** when changing core architecture
- **Always update** when adding new critical requirements
- **Update** when discovering important gotchas/pitfalls
- **Don't update** for minor bug fixes or refactors

#### Code Documentation
```cpp
// C++: Document non-obvious design decisions
// Why flat array instead of map?
// Answer: Cache locality critical for < 1μs updates
std::array<PriceLevel, MAX_LEVELS> levels_;

// Document all public APIs
/**
 * Apply delta to order book.
 *
 * @param delta The delta to apply
 * @return Result::SUCCESS or error code
 * @throws Never throws in hot path
 */
Result apply_delta(const Delta& delta);
```

```python
# Python: Docstrings for all public functions
def calculate_ofi(df: pl.DataFrame) -> pl.DataFrame:
    """
    Calculate Order Flow Imbalance.

    OFI = (bid_volume - ask_volume) / (bid_volume + ask_volume)

    Args:
        df: DataFrame with bid_sizes and ask_sizes columns

    Returns:
        DataFrame with additional 'ofi' column
    """
```

### Testing Requirements

Before committing to `main`:
- [ ] All unit tests pass
- [ ] All integration tests pass
- [ ] Code coverage maintained (>90% for core/, >70% for research/)
- [ ] Performance benchmarks pass (no regression)
- [ ] Linters pass with zero warnings

Before deploying to shadow:
- [ ] Full replay test passes
- [ ] No memory leaks (valgrind clean)
- [ ] Latency requirements met

Before deploying to live:
- [ ] Shadow trading runs for minimum 2 weeks
- [ ] Shadow fill rates match backtest within 10%
- [ ] Kill switch drill completed successfully
- [ ] All monitoring dashboards operational

## Development Workflow

### Development Guidelines
1. Avoid too many comments in the code, keep it clean and short
2. Always aim for production ready code, so we can iterate on a working env
3. Avoid creating too many files, more compact is more readable
4. Never mock data, always do the real implementation

### Adding New Exchange
1. Implement feed handler in `core/feeds/{exchange}/`
2. Handle exchange-specific snapshot/delta protocols
3. Add sequence validation and gap detection
4. Write integration test with recorded feed data in `tests/integration/`

### Adding New Alpha Signal
1. Research in `research/notebooks/` using Polars
2. Feature engineering in `research/features/`
3. IC/ICIR analysis in `research/alpha/`
4. Backtest with C++ matching core via `research/backtest/`
5. Walk-forward validation (crypto has frequent regime changes)

### Pre-Live Checklist
1. Unit tests pass for all modified modules
2. Integration tests validate feed → book → IPC chain
3. Replay tests confirm no regression
4. Shadow trading runs for minimum 2 weeks
5. Shadow slippage/fill rates match backtest assumptions

## Testing

**C++:** GTest for unit tests
**Python:** pytest for unit/integration
**Replay:** Store feed data and replay through full stack for regression testing

## Common Pitfalls

- Using `std::map` in order book (cache-unfriendly, use flat arrays)
- Backtesting at mid-price instead of replaying actual book
- Skipping or shortening shadow trading phase
- Divergent code paths between shadow and live
- Python in the hot path (execution/risk must be pure C++)
- Missing sequence number validation (silent book corruption)
