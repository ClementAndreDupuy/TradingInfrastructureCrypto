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

- Keep comments limited to invariants such as sequence handling, atomic visibility boundaries, and re-centering semantics; delete banner comments and obvious narrations of the control flow.
## Reusable Agent Memory (Updated)

- Orderbook edits require **explicit invariants** in code review notes/tests:
  - best bid < best ask violation handling
  - zero/negative size normalization
  - sequence reset behavior after reconnect
- Prefer deterministic replay fixtures (`tests/replay/`) whenever changing delta/snapshot logic.
- Any optimization must preserve all-or-nothing delta application semantics first, then latency.


## Classes & Methods (Quick Reference)

- **`OrderBook` (`orderbook.hpp`)** — Flat-array L2 book with dynamic grid and sequence-aware updates.
  - `apply_snapshot(const Snapshot&)`: Validates spread/checksum, resets grids, and loads full book state.
  - `apply_delta(const Delta&)`: Applies a single bid/ask level update and handles grid recenter behavior.
  - `get_best_bid()/get_best_ask()/get_mid_price()/get_spread()`: Returns top-of-book derived prices.
  - `get_top_levels(size_t, ...)`: Extracts top-N bid/ask levels for publishing or strategy use.
  - `tick_size()/max_levels()/active_levels()/base_price()`: Exposes current grid configuration and bounds.
