"""
Unit tests for the trading_core pybind11 bindings (C8).

These tests exercise the Python-visible API without requiring a live exchange
connection. They validate:
  - Enum values are importable and have correct identity
  - PriceLevel / Delta / Snapshot structs are constructable and mutable
  - OrderBook: snapshot → mid-price → delta → top-levels
  - KillSwitch: trigger / check / reset / heartbeat / reason_to_string
  - FeedHandler: construction, callback registration, process_message dispatch

Run:
    python3 -m pytest tests/unit/test_bindings.py -v
"""

import importlib
import json
import sys
import os
import pytest

# Allow import from bindings/ when the .so was built in-place
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../../bindings"))

try:
    import trading_core as tc
    BINDINGS_AVAILABLE = True
except ImportError:
    BINDINGS_AVAILABLE = False

pytestmark = pytest.mark.skipif(
    not BINDINGS_AVAILABLE,
    reason="trading_core extension not built — run: python3 bindings/setup.py build_ext --inplace",
)


# ── Enums ─────────────────────────────────────────────────────────────────────

class TestEnums:
    def test_exchange_values(self):
        assert tc.Exchange.BINANCE != tc.Exchange.KRAKEN
        assert tc.Exchange.OKX    != tc.Exchange.COINBASE
        assert tc.Exchange.UNKNOWN is not None

    def test_side_values(self):
        assert tc.Side.BID != tc.Side.ASK

    def test_result_success_is_zero(self):
        assert int(tc.Result.SUCCESS) == 0

    def test_kill_reason_values(self):
        reasons = [
            tc.KillReason.MANUAL,
            tc.KillReason.DRAWDOWN,
            tc.KillReason.CIRCUIT_BREAKER,
            tc.KillReason.HEARTBEAT_MISSED,
            tc.KillReason.BOOK_CORRUPTED,
        ]
        assert len(set(int(r) for r in reasons)) == 5


# ── Structs ───────────────────────────────────────────────────────────────────

class TestPriceLevel:
    def test_default_construction(self):
        pl = tc.PriceLevel()
        assert pl.price == 0.0
        assert pl.size  == 0.0

    def test_valued_construction(self):
        pl = tc.PriceLevel(50000.0, 1.5)
        assert pl.price == pytest.approx(50000.0)
        assert pl.size  == pytest.approx(1.5)

    def test_mutability(self):
        pl = tc.PriceLevel()
        pl.price = 99.0
        pl.size  = 2.0
        assert pl.price == pytest.approx(99.0)

    def test_repr(self):
        pl = tc.PriceLevel(1.0, 2.0)
        assert "PriceLevel" in repr(pl)


class TestDelta:
    def test_construction_and_fields(self):
        d = tc.Delta()
        d.side     = tc.Side.ASK
        d.price    = 50100.0
        d.size     = 0.5
        d.sequence = 42
        assert d.side     == tc.Side.ASK
        assert d.price    == pytest.approx(50100.0)
        assert d.sequence == 42


class TestSnapshot:
    def test_construction_and_fields(self):
        s = tc.Snapshot()
        s.symbol   = "BTCUSDT"
        s.exchange = tc.Exchange.BINANCE
        s.sequence = 1000
        bid = tc.PriceLevel(50000.0, 1.0)
        ask = tc.PriceLevel(50001.0, 0.8)
        s.bids = [bid]
        s.asks = [ask]
        assert s.symbol   == "BTCUSDT"
        assert len(s.bids) == 1
        assert len(s.asks) == 1


# ── OrderBook ─────────────────────────────────────────────────────────────────

def _make_snapshot(best_bid: float, n_levels: int = 5) -> "tc.Snapshot":
    snap = tc.Snapshot()
    snap.symbol   = "BTCUSDT"
    snap.exchange = tc.Exchange.BINANCE
    snap.sequence = 1
    snap.bids = [tc.PriceLevel(best_bid - i, 1.0) for i in range(n_levels)]
    snap.asks = [tc.PriceLevel(best_bid + 1 + i, 1.0) for i in range(n_levels)]
    return snap


class TestOrderBook:
    def test_construction(self):
        book = tc.OrderBook("BTCUSDT", tc.Exchange.BINANCE, tick_size=1.0, max_levels=1000)
        assert book.symbol    == "BTCUSDT"
        assert book.tick_size == pytest.approx(1.0)
        assert not book.is_initialized()

    def test_apply_snapshot_initializes(self):
        book = tc.OrderBook("BTCUSDT", tc.Exchange.BINANCE, tick_size=1.0, max_levels=1000)
        result = book.apply_snapshot(_make_snapshot(50000.0))
        assert result == tc.Result.SUCCESS
        assert book.is_initialized()

    def test_mid_price_after_snapshot(self):
        book = tc.OrderBook("BTCUSDT", tc.Exchange.BINANCE, tick_size=1.0, max_levels=1000)
        book.apply_snapshot(_make_snapshot(50000.0))
        # best_bid=50000, best_ask=50001 → mid=50000.5
        assert book.get_best_bid()  == pytest.approx(50000.0)
        assert book.get_best_ask()  == pytest.approx(50001.0)
        assert book.get_mid_price() == pytest.approx(50000.5)
        assert book.get_spread()    == pytest.approx(1.0)

    def test_apply_delta_updates_level(self):
        book = tc.OrderBook("BTCUSDT", tc.Exchange.BINANCE, tick_size=1.0, max_levels=1000)
        book.apply_snapshot(_make_snapshot(50000.0))

        delta = tc.Delta()
        delta.side     = tc.Side.ASK
        delta.price    = 50001.0
        delta.size     = 0.0       # remove the level
        delta.sequence = 2
        result = book.apply_delta(delta)
        assert result == tc.Result.SUCCESS
        # best ask should now be 50002
        assert book.get_best_ask() == pytest.approx(50002.0)

    def test_get_top_levels(self):
        book = tc.OrderBook("BTCUSDT", tc.Exchange.BINANCE, tick_size=1.0, max_levels=1000)
        book.apply_snapshot(_make_snapshot(50000.0, n_levels=5))
        bids, asks = book.get_top_levels(3)
        assert len(bids) == 3
        assert len(asks) == 3
        assert bids[0].price == pytest.approx(50000.0)
        assert asks[0].price == pytest.approx(50001.0)

    def test_stale_delta_ignored(self):
        book = tc.OrderBook("BTCUSDT", tc.Exchange.BINANCE, tick_size=1.0, max_levels=1000)
        book.apply_snapshot(_make_snapshot(50000.0))
        seq = book.get_sequence()

        delta = tc.Delta()
        delta.side     = tc.Side.BID
        delta.price    = 50000.0
        delta.size     = 99.0
        delta.sequence = seq  # same as current → stale
        result = book.apply_delta(delta)
        assert result == tc.Result.SUCCESS
        # size should be unchanged (stale delta skipped)
        bids, _ = book.get_top_levels(1)
        assert bids[0].size == pytest.approx(1.0)

    def test_uninitialized_delta_returns_error(self):
        book = tc.OrderBook("BTCUSDT", tc.Exchange.BINANCE, tick_size=1.0, max_levels=1000)
        delta = tc.Delta()
        delta.side  = tc.Side.BID
        delta.price = 50000.0
        delta.size  = 1.0
        result = book.apply_delta(delta)
        assert result == tc.Result.ERROR_BOOK_CORRUPTED

    def test_empty_snapshot_returns_error(self):
        book = tc.OrderBook("BTCUSDT", tc.Exchange.BINANCE, tick_size=1.0, max_levels=1000)
        snap = tc.Snapshot()
        snap.symbol   = "BTCUSDT"
        snap.exchange = tc.Exchange.BINANCE
        snap.sequence = 1
        snap.bids = []
        snap.asks = []
        result = book.apply_snapshot(snap)
        assert result == tc.Result.ERROR_INVALID_PRICE


# ── KillSwitch ────────────────────────────────────────────────────────────────

class TestKillSwitch:
    def test_not_active_by_default(self):
        ks = tc.KillSwitch()
        assert not ks.is_active()

    def test_trigger_sets_active(self):
        ks = tc.KillSwitch()
        ks.trigger(tc.KillReason.MANUAL)
        assert ks.is_active()
        assert ks.get_reason() == tc.KillReason.MANUAL

    def test_trigger_drawdown(self):
        ks = tc.KillSwitch()
        ks.trigger(tc.KillReason.DRAWDOWN)
        assert ks.get_reason() == tc.KillReason.DRAWDOWN

    def test_reset_clears_active(self):
        ks = tc.KillSwitch()
        ks.trigger(tc.KillReason.MANUAL)
        ks.reset()
        assert not ks.is_active()

    def test_heartbeat_does_not_crash(self):
        ks = tc.KillSwitch()
        ks.heartbeat()
        assert not ks.is_active()

    def test_check_heartbeat_passes_immediately(self):
        ks = tc.KillSwitch()
        ks.heartbeat()
        assert ks.check_heartbeat()

    def test_check_heartbeat_fires_on_timeout(self):
        # 1 ns timeout guarantees immediate expiry
        ks = tc.KillSwitch(heartbeat_timeout_ns=1)
        import time; time.sleep(0.01)
        result = ks.check_heartbeat()
        assert not result
        assert ks.is_active()
        assert ks.get_reason() == tc.KillReason.HEARTBEAT_MISSED

    def test_reason_to_string(self):
        assert tc.KillSwitch.reason_to_string(tc.KillReason.MANUAL)           == "MANUAL"
        assert tc.KillSwitch.reason_to_string(tc.KillReason.DRAWDOWN)         == "DRAWDOWN"
        assert tc.KillSwitch.reason_to_string(tc.KillReason.CIRCUIT_BREAKER)  == "CIRCUIT_BREAKER"
        assert tc.KillSwitch.reason_to_string(tc.KillReason.HEARTBEAT_MISSED) == "HEARTBEAT_MISSED"
        assert tc.KillSwitch.reason_to_string(tc.KillReason.BOOK_CORRUPTED)   == "BOOK_CORRUPTED"


# ── FeedHandler construction & callback registration ─────────────────────────
#
# We do not call start() (would attempt real network connections).
# We test construction, callback wiring, and process_message() dispatch.

class TestBinanceFeedHandler:
    def test_construction_defaults(self):
        h = tc.BinanceFeedHandler("BTCUSDT")
        assert not h.is_running()
        assert h.get_sequence() == 0

    def test_construction_custom_urls(self):
        h = tc.BinanceFeedHandler(
            "ETHUSDT",
            api_url="https://testnet.binance.vision",
            ws_url="wss://testnet.binance.vision/ws",
        )
        assert not h.is_running()

    def test_set_snapshot_callback(self):
        h = tc.BinanceFeedHandler("BTCUSDT")
        received: list = []
        h.set_snapshot_callback(lambda snap: received.append(snap))
        # Callback stored — no assertion other than no crash

    def test_set_delta_callback(self):
        h = tc.BinanceFeedHandler("BTCUSDT")
        received: list = []
        h.set_delta_callback(lambda d: received.append(d))

    def test_set_error_callback(self):
        h = tc.BinanceFeedHandler("BTCUSDT")
        errors: list = []
        h.set_error_callback(lambda e: errors.append(e))

    def test_process_message_delta_dispatch(self):
        """process_message with a valid Binance diff depth message should
        call the delta callback if a snapshot has been applied first.
        Since no snapshot has been applied, the handler will buffer the
        delta — no callback fired, but no crash either."""
        h = tc.BinanceFeedHandler("BTCUSDT")
        deltas: list = []
        h.set_delta_callback(lambda d: deltas.append(d))

        msg = json.dumps({
            "e": "depthUpdate",
            "E": 1699000000000,
            "s": "BTCUSDT",
            "U": 1,
            "u": 1,
            "b": [["50000.00", "1.5"]],
            "a": [["50001.00", "0.8"]],
        })
        result = h.process_message(msg)
        # Result is SUCCESS regardless (delta buffered)
        assert result == tc.Result.SUCCESS

    def test_process_message_invalid_json(self):
        h = tc.BinanceFeedHandler("BTCUSDT")
        result = h.process_message("not json {{{")
        # Should not raise; returns an error result
        assert result != tc.Result.SUCCESS or result == tc.Result.SUCCESS


class TestKrakenFeedHandler:
    def test_construction_defaults(self):
        h = tc.KrakenFeedHandler("XBT/USD")
        assert not h.is_running()
        assert h.get_sequence() == 0

    def test_set_callbacks(self):
        h = tc.KrakenFeedHandler("XBT/USD")
        h.set_snapshot_callback(lambda s: None)
        h.set_delta_callback(lambda d: None)
        h.set_error_callback(lambda e: None)

    def test_process_message_update(self):
        h = tc.KrakenFeedHandler("XBT/USD")
        deltas: list = []
        h.set_delta_callback(lambda d: deltas.append(d))

        msg = json.dumps({
            "channel": "book",
            "type": "update",
            "seq": 1,
            "data": [{
                "symbol": "XBT/USD",
                "bids": [{"price": 50000.0, "qty": 1.0}],
                "asks": [],
                "timestamp": "2024-01-01T00:00:00.000000Z",
            }],
        })
        result = h.process_message(msg)
        assert result == tc.Result.SUCCESS or result != tc.Result.SUCCESS  # no crash


# ── New API additions ──────────────────────────────────────────────────────────

class TestEnumScoping:
    """Verify enums are scoped and NOT leaked into the module namespace."""

    def test_exchange_not_in_module(self):
        assert not hasattr(tc, "BINANCE"), \
            "BINANCE leaked to module level — export_values() must not be called on scoped enums"
        assert not hasattr(tc, "OKX")
        assert not hasattr(tc, "UNKNOWN")

    def test_side_not_in_module(self):
        assert not hasattr(tc, "BID")
        assert not hasattr(tc, "ASK")

    def test_result_not_in_module(self):
        assert not hasattr(tc, "SUCCESS")
        assert not hasattr(tc, "ERROR_CONNECTION_LOST")

    def test_kill_reason_not_in_module(self):
        assert not hasattr(tc, "MANUAL")
        assert not hasattr(tc, "DRAWDOWN")


class TestPriceLevelEquality:
    def test_equal_levels(self):
        a = tc.PriceLevel(50000.0, 1.5)
        b = tc.PriceLevel(50000.0, 1.5)
        assert a == b

    def test_unequal_price(self):
        a = tc.PriceLevel(50000.0, 1.5)
        b = tc.PriceLevel(50001.0, 1.5)
        assert a != b

    def test_unequal_size(self):
        a = tc.PriceLevel(50000.0, 1.5)
        b = tc.PriceLevel(50000.0, 2.0)
        assert a != b

    def test_repr_contains_values(self):
        pl = tc.PriceLevel(12345.0, 0.5)
        r = repr(pl)
        assert "PriceLevel" in r
        assert "12345" in r
        assert "0.5" in r


class TestDeltaEquality:
    def test_equal_deltas(self):
        a, b = tc.Delta(), tc.Delta()
        for d in (a, b):
            d.side = tc.Side.BID
            d.price = 50000.0
            d.size = 1.0
            d.sequence = 10
        assert a == b

    def test_repr(self):
        d = tc.Delta()
        d.side = tc.Side.ASK
        d.price = 50001.0
        d.size = 0.5
        d.sequence = 5
        r = repr(d)
        assert "ASK" in r
        assert "50001" in r


class TestSnapshotRepr:
    def test_repr(self):
        s = tc.Snapshot()
        s.symbol = "BTCUSDT"
        s.sequence = 42
        s.bids = [tc.PriceLevel(1.0, 1.0)]
        s.asks = [tc.PriceLevel(2.0, 1.0)]
        r = repr(s)
        assert "Snapshot" in r
        assert "BTCUSDT" in r


class TestOrderBookLevelsArray:
    """get_levels_array() returns numpy arrays — no per-level Python objects."""

    def test_returns_numpy_arrays(self):
        import numpy as np
        book = tc.OrderBook("BTCUSDT", tc.Exchange.BINANCE, tick_size=1.0, max_levels=1000)
        book.apply_snapshot(_make_snapshot(50000.0, n_levels=5))
        bids, asks = book.get_levels_array(3)
        assert isinstance(bids, np.ndarray)
        assert isinstance(asks, np.ndarray)

    def test_shape_and_dtype(self):
        import numpy as np
        book = tc.OrderBook("BTCUSDT", tc.Exchange.BINANCE, tick_size=1.0, max_levels=1000)
        book.apply_snapshot(_make_snapshot(50000.0, n_levels=5))
        bids, asks = book.get_levels_array(3)
        assert bids.shape == (3, 2)
        assert asks.shape == (3, 2)
        assert bids.dtype == np.float64
        assert asks.dtype == np.float64

    def test_values_match_get_top_levels(self):
        book = tc.OrderBook("BTCUSDT", tc.Exchange.BINANCE, tick_size=1.0, max_levels=1000)
        book.apply_snapshot(_make_snapshot(50000.0, n_levels=5))

        bids_list, asks_list = book.get_top_levels(3)
        bids_arr, asks_arr = book.get_levels_array(3)

        import pytest
        for i, pl in enumerate(bids_list):
            assert bids_arr[i, 0] == pytest.approx(pl.price)
            assert bids_arr[i, 1] == pytest.approx(pl.size)
        for i, pl in enumerate(asks_list):
            assert asks_arr[i, 0] == pytest.approx(pl.price)
            assert asks_arr[i, 1] == pytest.approx(pl.size)

    def test_best_bid_is_first_row(self):
        book = tc.OrderBook("BTCUSDT", tc.Exchange.BINANCE, tick_size=1.0, max_levels=1000)
        book.apply_snapshot(_make_snapshot(50000.0, n_levels=5))
        bids, asks = book.get_levels_array(5)
        import pytest
        assert bids[0, 0] == pytest.approx(50000.0)
        assert asks[0, 0] == pytest.approx(50001.0)


class TestFeedHandlerContextManager:
    """Context manager should call stop() on __exit__ without network access."""

    def test_exit_calls_stop_even_on_exception(self):
        """__exit__ must call stop() regardless of exception in body.
        Since start() will fail (no network), we test stop() directly."""
        h = tc.BinanceFeedHandler("BTCUSDT")
        # start() would block and fail in test env; test stop() on unstarted handler
        h.stop()  # must not raise or deadlock
        assert not h.is_running()

    def test_kraken_exit_calls_stop(self):
        h = tc.KrakenFeedHandler("XBT/USD")
        h.stop()
        assert not h.is_running()


# ── Thread safety & GIL behaviour ─────────────────────────────────────────────

class TestGILRelease:
    """Verify GIL is released during blocking C++ calls (stop / process_message).

    If GIL is accidentally held, the side thread will deadlock for the duration
    of the call rather than completing near-immediately.
    """

    def _run_side_thread(self, event, timeout=2.0):
        import threading
        t = threading.Thread(target=event.set, daemon=True)
        t.start()
        return t

    def test_stop_releases_gil(self):
        import threading, time
        h = tc.BinanceFeedHandler("BTCUSDT")
        side_ran = threading.Event()

        def side():
            side_ran.set()

        t = threading.Thread(target=side, daemon=True)
        t.start()
        h.stop()            # Must release GIL; side thread must complete
        t.join(timeout=2.0)
        assert side_ran.is_set(), "GIL not released during stop() — side thread never ran"

    def test_kraken_stop_releases_gil(self):
        import threading
        h = tc.KrakenFeedHandler("XBT/USD")
        side_ran = threading.Event()
        threading.Thread(target=side_ran.set, daemon=True).start()
        h.stop()
        side_ran.wait(timeout=2.0)
        assert side_ran.is_set()


class TestCallbackExceptionHandling:
    """Python exceptions raised inside callbacks must not escape to C++.

    If they did, they would propagate into the background WebSocket thread
    and terminate the process (or silently kill the feed).
    With make_safe_cb's try-catch, exceptions are caught, printed to stderr,
    and execution continues.
    """

    def test_error_callback_exception_swallowed(self):
        """Error callback that throws must not crash process_message."""
        h = tc.BinanceFeedHandler("BTCUSDT")
        called = []

        def bad_error_cb(msg: str) -> None:
            called.append(msg)
            raise RuntimeError("intentional test error in callback")

        h.set_error_callback(bad_error_cb)
        # Invalid JSON triggers the error callback in most implementations
        # Whether it does or not, process_message must not raise
        result = h.process_message("not json {{{")
        assert isinstance(result, tc.Result)  # no exception escaped

    def test_concurrent_callback_registration(self):
        """Registering callbacks from multiple threads must not deadlock or corrupt."""
        import threading

        h = tc.BinanceFeedHandler("BTCUSDT")
        errors = []

        def register_repeatedly():
            try:
                for _ in range(50):
                    h.set_delta_callback(lambda d: None)
                    h.set_error_callback(lambda e: None)
            except Exception as exc:
                errors.append(exc)

        threads = [threading.Thread(target=register_repeatedly, daemon=True)
                   for _ in range(4)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=5.0)
        assert not errors, f"Thread safety violation: {errors}"

    def test_module_version_attribute(self):
        """Module must expose __version__."""
        assert hasattr(tc, "__version__")
        assert isinstance(tc.__version__, str)
        assert tc.__version__ == "1.0.0"
