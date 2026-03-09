# tests/

Testing strategy for a high-frequency trading system.

## Test Pyramid

```
    Replay Tests (few, slow, comprehensive)
         /\
        /  \
  Integration Tests (some, medium speed)
      /      \
     /        \
Unit Tests (many, fast, focused)
```

## Unit Tests (`unit/`)

**Fast, isolated, deterministic.**

### C++ Unit Tests (GTest)
```cpp
// tests/unit/orderbook_test.cpp
TEST(OrderBook, ApplyBidDelta) {
    OrderBook book;
    book.initialize_snapshot(snapshot);

    Delta delta{.side = BID, .price = 50000, .size = 1.5};
    book.apply_delta(delta);

    EXPECT_EQ(book.get_bid_size(50000), 1.5);
}

TEST(OrderBook, SequenceGapTriggersSnapshot) {
    OrderBook book;
    book.set_sequence(100);

    Delta delta{.sequence = 102};  // Gap!
    EXPECT_THROW(book.apply_delta(delta), SequenceGapError);
}
```

Build and run:
```bash
cd build
make orderbook_test
./tests/unit/orderbook_test
```

### Python Unit Tests (pytest)
```python
# tests/unit/test_features.py
import polars as pl
from research.features import calculate_ofi

def test_ofi_balanced_book():
    df = pl.DataFrame({
        'bid_sizes': [[1.0, 2.0]],
        'ask_sizes': [[1.0, 2.0]],
    })

    result = calculate_ofi(df)
    assert result['ofi'][0] == 0.0  # Balanced = 0

def test_ofi_bid_heavy():
    df = pl.DataFrame({
        'bid_sizes': [[3.0, 2.0]],
        'ask_sizes': [[1.0, 1.0]],
    })

    result = calculate_ofi(df)
    assert result['ofi'][0] > 0  # More bid volume = positive OFI
```

Run:
```bash
pytest tests/unit/ -v
```

## Integration Tests (`integration/`)

**Test module interactions, slower than unit tests.**

```cpp
// tests/integration/feed_to_book_test.cpp
TEST(Integration, BinanceFeedUpdatesBook) {
    // Real Binance WebSocket message
    std::string msg = R"({
        "e": "depthUpdate",
        "s": "BTCUSDT",
        "U": 100,
        "u": 100,
        "b": [["50000", "1.5"]],
        "a": []
    })";

    BinanceFeedHandler handler;
    OrderBook book;

    handler.on_message(msg, book);

    EXPECT_EQ(book.get_bid_size(50000), 1.5);
    EXPECT_EQ(book.get_sequence(), 100);
}
```

```python
# tests/integration/test_backtest_pipeline.py
def test_end_to_end_backtest():
    # Load sample data
    df = pl.read_parquet('tests/data/sample_ticks.parquet')

    # Run backtest
    from research.backtest import BacktestEngine
    engine = BacktestEngine(latency_ns=1000)
    results = engine.run(df, strategy='simple_ofi')

    # Verify output structure
    assert 'pnl' in results
    assert 'fill_rate' in results
    assert len(results['trades']) > 0
```

## Replay Tests (`replay/`)

**Full stack regression testing with recorded data.**

```python
# tests/replay/test_binance_replay.py
def test_replay_binance_20240315():
    """
    Replay full day of Binance data through:
    feed handler → order book → matching engine
    Compare final book state to known-good snapshot.
    """
    # Load recorded WebSocket messages
    messages = load_recorded_feed('tests/data/binance_BTCUSDT_20240315.jsonl')

    # Replay through system
    book = OrderBook()
    for msg in messages:
        feed_handler.process(msg, book)

    # Load expected final state
    expected = load_snapshot('tests/data/binance_BTCUSDT_20240315_final.json')

    # Compare
    assert book.to_snapshot() == expected
```

**Critical:** Replay tests catch silent divergence bugs that unit tests miss.

## Test Data

Store real feed data for testing:
```bash
tests/data/
├── binance_BTCUSDT_20240315.jsonl       # Full day of WebSocket messages
├── binance_BTCUSDT_20240315_final.json  # Expected final book state
├── okx_BTCUSDT_20240315.jsonl
└── sample_ticks.parquet                 # Small sample for fast tests
```

Record live data:
```bash
# Run feed recorder
./scripts/record_feed.py --exchange binance --symbol BTC-USD --duration 1d
```

## Testing Kill Switch

**Weekly drill:**
```bash
# 1. Trigger software kill switch
./scripts/trigger_kill_switch.sh

# 2. Verify all orders canceled
./scripts/verify_no_open_orders.sh

# 3. Verify no new orders can be submitted
./tests/integration/test_kill_switch.cpp

# 4. Reset kill switch
./scripts/reset_kill_switch.sh
```

## Performance Tests

Benchmark critical paths:
```cpp
// tests/performance/orderbook_bench.cpp
BENCHMARK(OrderBookDeltaApplication) {
    OrderBook book;
    book.initialize_snapshot(snapshot);

    for (int i = 0; i < 1000000; ++i) {
        Delta delta = generate_random_delta();
        book.apply_delta(delta);
    }
}
// Target: < 500ns per delta
```

Run:
```bash
./tests/performance/orderbook_bench
```

## CI/CD

All tests run on every commit:
```yaml
# .github/workflows/ci.yml
- name: C++ Unit Tests
  run: |
    cd build
    make test

- name: Python Unit Tests
  run: pytest tests/unit/

- name: Integration Tests
  run: pytest tests/integration/

- name: Replay Tests (nightly)
  if: github.event_name == 'schedule'
  run: pytest tests/replay/ --timeout=3600
```

## Test Coverage

Minimum coverage requirements:
- **core/**: 90%+ (critical path)
- **research/**: 70%+ (less critical)

Check coverage:
```bash
# C++
gcov -r core/
lcov --capture --directory . --output-file coverage.info

# Python
pytest --cov=research tests/
```
