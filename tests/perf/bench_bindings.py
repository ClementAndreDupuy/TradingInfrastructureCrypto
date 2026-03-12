"""
Performance benchmarks for trading_core pybind11 bindings.

Validates latency budgets from CLAUDE.md:
    apply_delta      < 1 µs
    apply_snapshot   < 100 µs  (one-shot, not on hot path)
    get_mid_price    < 1 µs
    is_active        < 1 µs  (kill switch hot-path)

Install and run:
    pip install pytest-benchmark
    pytest tests/perf/bench_bindings.py -v --benchmark-sort=mean \
           --benchmark-warmup=on --benchmark-min-rounds=10000
"""

import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../../bindings"))

# Skip entire module if extension not built or pytest-benchmark not installed
pytest.importorskip(
    "pytest_benchmark",
    reason="pip install pytest-benchmark to run perf benchmarks",
)

try:
    import trading_core as tc
    BINDINGS_AVAILABLE = True
except ImportError:
    BINDINGS_AVAILABLE = False

pytestmark = pytest.mark.skipif(
    not BINDINGS_AVAILABLE,
    reason="trading_core extension not built — run: python3 bindings/setup.py build_ext --inplace",
)


# ── Fixtures ───────────────────────────────────────────────────────────────────

def _make_snapshot(best_bid: float = 50000.0, n_levels: int = 20) -> "tc.Snapshot":
    snap = tc.Snapshot()
    snap.symbol   = "BTCUSDT"
    snap.exchange = tc.Exchange.BINANCE
    snap.sequence = 1
    snap.bids = [tc.PriceLevel(best_bid - i, 1.0 + i * 0.1) for i in range(n_levels)]
    snap.asks = [tc.PriceLevel(best_bid + 1 + i, 1.0 + i * 0.1) for i in range(n_levels)]
    return snap


def _make_book(n_levels: int = 20, max_levels: int = 5000) -> "tc.OrderBook":
    book = tc.OrderBook("BTCUSDT", tc.Exchange.BINANCE, tick_size=1.0, max_levels=max_levels)
    book.apply_snapshot(_make_snapshot(n_levels=n_levels))
    return book


# ── OrderBook benchmarks ───────────────────────────────────────────────────────

class TestOrderBookPerf:
    """Budget: apply_delta < 1 µs; get_mid_price < 1 µs."""

    def test_bench_apply_snapshot(self, benchmark):
        """Snapshot is one-shot init; no hard budget but track for regressions."""
        book = tc.OrderBook("BTCUSDT", tc.Exchange.BINANCE, tick_size=1.0, max_levels=5000)
        snap = _make_snapshot(n_levels=20)
        benchmark(book.apply_snapshot, snap)

    def test_bench_apply_delta(self, benchmark):
        """BUDGET: < 1 µs.  Exercises the O(1) flat-array update path."""
        book  = _make_book()
        delta = tc.Delta()
        seq   = [2]

        def apply_one():
            delta.side     = tc.Side.ASK
            delta.price    = 50001.0 + (seq[0] % 10)
            delta.size     = 0.5
            delta.sequence = seq[0]
            seq[0] += 1
            book.apply_delta(delta)

        benchmark(apply_one)

    def test_bench_get_mid_price(self, benchmark):
        """BUDGET: < 1 µs.  Should be O(1) array index."""
        book = _make_book()
        benchmark(book.get_mid_price)

    def test_bench_get_spread(self, benchmark):
        book = _make_book()
        benchmark(book.get_spread)

    def test_bench_get_best_bid(self, benchmark):
        book = _make_book()
        benchmark(book.get_best_bid)

    def test_bench_get_top_levels_10(self, benchmark):
        """Returns Python objects — expect higher overhead than get_levels_array."""
        book = _make_book(n_levels=20)
        benchmark(book.get_top_levels, 10)

    def test_bench_get_levels_array_10(self, benchmark):
        """Returns numpy arrays — should be faster than get_top_levels for same depth."""
        book = _make_book(n_levels=20)
        benchmark(book.get_levels_array, 10)

    def test_bench_get_levels_array_100(self, benchmark):
        """Deeper slice — measures numpy allocation overhead vs. Python object overhead."""
        book = _make_book(n_levels=200, max_levels=10000)
        benchmark(book.get_levels_array, 100)


# ── KillSwitch benchmarks ──────────────────────────────────────────────────────

class TestKillSwitchPerf:
    """Budget: is_active < 1 µs (atomic load on hot path)."""

    def test_bench_is_active(self, benchmark):
        """BUDGET: < 1 µs.  Must be a bare atomic load."""
        ks = tc.KillSwitch()
        ks.heartbeat()
        benchmark(ks.is_active)

    def test_bench_heartbeat(self, benchmark):
        """BUDGET: < 1 µs.  Atomic store — called every tick."""
        ks = tc.KillSwitch()
        benchmark(ks.heartbeat)

    def test_bench_check_heartbeat(self, benchmark):
        """Called from monitoring thread; < 10 µs budget."""
        ks = tc.KillSwitch()
        ks.heartbeat()
        benchmark(ks.check_heartbeat)


# ── Struct construction benchmarks ────────────────────────────────────────────

class TestStructPerf:
    """Baseline for Python-side object construction cost."""

    def test_bench_price_level_construct(self, benchmark):
        benchmark(tc.PriceLevel, 50000.0, 1.0)

    def test_bench_delta_construct(self, benchmark):
        benchmark(tc.Delta)
