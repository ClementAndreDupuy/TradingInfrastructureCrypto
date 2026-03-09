# core/

**C++ hot path components** - All latency-critical code lives here.

## Critical Rules

1. **No Python** - Never call Python from this layer
2. **No heap allocations in hot path** - Pre-allocate everything at startup
3. **Cache-friendly data structures** - Prefer flat arrays over `std::map`/`std::unordered_map`
4. **Timestamping** - Use PTP/IEEE 1588, never `std::chrono::system_clock`
5. **Error handling** - Log and fail fast, don't silently continue with bad state

## Components

- **common/** - Shared types, enums, constants (e.g., `Exchange`, `Side`, `OrderType`)
- **orderbook/** - The most critical component - all downstream depends on book integrity
- **feeds/** - Per-exchange WebSocket handlers (Binance, OKX, Coinbase)
- **ipc/** - Shared memory, ring buffers for publishing to Python layer
- **risk/** - Pre-trade checks, must be sub-microsecond
- **execution/** - Order manager, exchange connectors
- **shadow/** - Paper trading with identical code path to live

## Build & Test

```bash
cd build
cmake ..
make -j$(nproc)
./tests/unit/core_test
```

## Performance Requirements

- Order book update: < 1 microsecond
- Risk check: < 1 microsecond
- Feed handler latency: < 10 microseconds from network receipt to book update

## Dependencies

- Boost.Asio for async I/O
- libwebsockets for WebSocket connections
- Custom memory allocators for zero-allocation hot path