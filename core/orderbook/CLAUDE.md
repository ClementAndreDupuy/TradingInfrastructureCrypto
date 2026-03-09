# core/orderbook/

**THE MOST CRITICAL COMPONENT** - Everything downstream depends on book integrity.

## Architecture

- **Flat array indexed by price tick grid** - NOT `std::map` (cache-unfriendly)
- **Normalized L2/L3 representation** - Unified interface for all exchanges
- **Zero-allocation updates** - All memory pre-allocated at startup
- **Sequence number tracking** - Detect gaps, trigger re-snapshot on miss

## Key Files (to be implemented)

- `orderbook.hpp/cpp` - Core book structure and operations
- `book_snapshot.hpp/cpp` - Snapshot handling
- `book_delta.hpp/cpp` - Delta application logic
- `price_grid.hpp/cpp` - Price tick grid mapping

## Critical Requirements

### 1. Sequence Number Validation
Every delta MUST be validated:
```cpp
if (msg.sequence != expected_sequence) {
    log_error("Gap detected: expected {}, got {}", expected_sequence, msg.sequence);
    request_snapshot();
    return;
}
```

### 2. Delta Application
Must be atomic - either fully apply or reject:
```cpp
// WRONG: Partial application
book.update_bid(price, qty);  // If this succeeds...
book.update_ask(price, qty);  // ...but this fails, book is corrupt

// RIGHT: Validate first, apply atomically
if (validate_delta(delta)) {
    apply_delta(delta);  // All-or-nothing
}
```

### 3. Timestamp Consistency
Use PTP hardware timestamps, not system clock:
```cpp
// WRONG
auto ts = std::chrono::system_clock::now();

// RIGHT
auto ts = ptp_clock::now();  // Hardware timestamped at NIC
```

## Testing Strategy

1. **Unit tests** - Individual delta operations
2. **Replay tests** - Feed recorded exchange data, compare final book state
3. **Fuzz testing** - Random deltas, ensure no crashes or corruption
4. **Gap injection** - Simulate missed messages, verify snapshot recovery

## Common Bugs to Avoid

- **Silent divergence** - Book drifts from exchange but no error detected
- **Price level leaks** - Failed to remove empty levels, memory grows unbounded
- **Race conditions** - Multiple threads updating book without proper synchronization
- **Integer overflow** - Quantity accumulation without bounds checking

## Performance Target

- **Delta application**: < 500 nanoseconds per update
- **Book snapshot**: < 10 microseconds
- **Memory footprint**: < 100MB per symbol per exchange