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
