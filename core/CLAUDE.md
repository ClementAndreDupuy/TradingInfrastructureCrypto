# core/

**C++ hot path** — all latency-critical code lives here.

## Rules

1. No Python — never call Python from this layer.
2. No heap allocation in hot path — pre-allocate everything at startup.
3. Cache-friendly structures — flat arrays over `std::map`.
4. PTP timestamps — never `std::chrono::system_clock`.
5. Fail fast — log and halt on bad state, never silently continue.

## Components

- **common/** — Shared types, enums, constants
- **orderbook/** — The most critical component; all downstream depends on correctness
- **feeds/** — Per-exchange WebSocket handlers
- **ipc/** — Shared memory, ring buffers (LMAX Disruptor pattern)
- **risk/** — Pre-trade checks, kill switch (< 1 µs)
- **execution/** — Order manager, exchange connectors
- **shadow/** — Paper trading with identical code path to live
- **strategy/** — Strategy implementations

## Performance Requirements

- Order book update: < 1 µs
- Risk check: < 1 µs
- Feed handler: < 10 µs end-to-end
