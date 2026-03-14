# core/orderbook/

**Most critical component.** All downstream depends on book integrity.

## Architecture

Flat array indexed by price tick grid — not `std::map`. Normalized L2 representation. Zero-allocation updates (all memory pre-allocated at startup). Sequence number tracking with gap detection and re-snapshot trigger.

## Critical Requirements

1. **Sequence validation** — every delta must be checked; gap → request snapshot immediately.
2. **Atomic delta application** — validate first, apply all-or-nothing.
3. **Hardware timestamps** — use PTP/IEEE 1588, never `std::chrono::system_clock`.
4. **Silent divergence is the worst failure** — book drifts without error detection.

## Performance Target

- Delta application: < 500 ns
- Snapshot: < 10 µs
- Memory: < 100 MB per symbol per exchange

## Testing

Unit tests → replay tests (recorded feed, compare final state) → fuzz tests → gap injection (verify snapshot recovery).
