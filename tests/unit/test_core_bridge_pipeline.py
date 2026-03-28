from __future__ import annotations

import mmap
import struct
import tempfile
from pathlib import Path

import pytest

ROOT = Path(__file__).parent.parent.parent
import sys

sys.path.insert(0, str(ROOT))

from research.neural_alpha.runtime.core_bridge import CoreBridge

try:
    import trading_core as tc

    _HAS_TC = True
except ImportError:
    tc = None  # type: ignore[assignment]
    _HAS_TC = False

try:
    import polars as pl
except ModuleNotFoundError:  # pragma: no cover - environment-dependent
    pl = None

if pl is not None:
    from research.neural_alpha import pipeline
else:
    pipeline = None

HEADER_FMT = "<8sIIIIQ32s"
SLOT_FMT = "<B15sqd" + "d" * 40 + "I" * 20 + "B7s"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
SLOT_SIZE = struct.calcsize(SLOT_FMT)
CAPACITY = 8


def _write_ring(
    path: Path,
    write_seq: int,
    slots: dict[int, tuple[int, str, int, float, int]],
) -> None:
    total_size = HEADER_SIZE + CAPACITY * SLOT_SIZE
    with path.open("w+b") as f:
        f.truncate(total_size)
        mm = mmap.mmap(f.fileno(), total_size)
        header = struct.pack(
            HEADER_FMT,
            b"TRTLOB01",
            1,
            SLOT_SIZE,
            CAPACITY,
            0,
            write_seq,
            b"\x00" * 32,
        )
        mm[0:HEADER_SIZE] = header

        for seq, (exchange_id, symbol, ts_ns, mid, trade_direction) in slots.items():
            offset = HEADER_SIZE + (seq % CAPACITY) * SLOT_SIZE
            symbol_bytes = symbol.encode("utf-8")[:15].ljust(15, b"\x00")
            bids = [mid - 1 - i * 0.1 for i in range(10)]
            bid_sizes = [1.0 + i for i in range(10)]
            asks = [mid + 1 + i * 0.1 for i in range(10)]
            ask_sizes = [1.5 + i for i in range(10)]
            payload = struct.pack(
                SLOT_FMT,
                exchange_id,
                symbol_bytes,
                ts_ns,
                mid,
                *bids,
                *bid_sizes,
                *asks,
                *ask_sizes,
                *([3] * 10),
                *([4] * 10),
                trade_direction,
                b"\x00" * 7,
            )
            mm[offset : offset + SLOT_SIZE] = payload
        mm.flush()
        mm.close()


def test_core_bridge_open_false_when_missing() -> None:
    bridge = CoreBridge(path="/tmp/does_not_exist_trt_lob_feed.bin")
    assert bridge.open() is False


def test_core_bridge_reads_rows_and_drops_overflowed_slots() -> None:
    with tempfile.TemporaryDirectory() as td:
        ring_path = Path(td) / "lob.bin"
        slots = {
            8: (0, "BTCUSDT", 1_000, 100.0, 1),
            9: (3, "XBTUSD", 2_000, 101.0, 0),
            10: (1, "BTC-USDT", 3_000, 102.0, 1),
        }
        _write_ring(ring_path, write_seq=11, slots=slots)

        bridge = CoreBridge(path=str(ring_path))
        assert bridge.open() is True
        bridge._last_seq = 0  # simulate stale reader before overflow window

        rows = bridge.read_new_ticks()
        bridge.close()

        assert len(rows) == 8  # write_seq=11 and capacity=8 -> valid seq [3..10]
        assert rows[-1]["exchange"] == "OKX"
        assert rows[-1]["symbol"] == "BTC-USDT"
        assert rows[-1]["timestamp_ns"] == 3_000
        assert rows[-1]["trade_direction"] == 1
        assert "bid_price_10" in rows[-1]
        assert "ask_size_10" in rows[-1]
        assert rows[-1]["bid_oc_1"] == 3
        assert rows[-1]["ask_oc_1"] == 4


@pytest.mark.skipif(pl is None or pipeline is None, reason="polars is not installed")
def test_collect_l5_ticks_prefers_bridge_then_tops_up(monkeypatch) -> None:
    assert pl is not None
    bridge_df = pl.DataFrame(
        [
            {
                "timestamp_ns": 1,
                "exchange": "BINANCE",
                "symbol": "BTCUSDT",
                "best_bid": 1.0,
                "best_ask": 2.0,
                "trade_direction": 1,
                **{f"bid_price_{i}": 1.0 for i in range(1, 11)},
                **{f"bid_size_{i}": 1.0 for i in range(1, 11)},
                **{f"ask_price_{i}": 2.0 for i in range(1, 11)},
                **{f"ask_size_{i}": 1.0 for i in range(1, 11)},
            },
            {
                "timestamp_ns": 2,
                "exchange": "KRAKEN",
                "symbol": "XBTUSD",
                "best_bid": 1.1,
                "best_ask": 2.1,
                "trade_direction": 0,
                **{f"bid_price_{i}": 1.1 for i in range(1, 11)},
                **{f"bid_size_{i}": 1.0 for i in range(1, 11)},
                **{f"ask_price_{i}": 2.1 for i in range(1, 11)},
                **{f"ask_size_{i}": 1.0 for i in range(1, 11)},
            },
        ]
    )

    rest_df = pl.DataFrame(
        [
            {
                "timestamp_ns": 3,
                "exchange": "OKX",
                "symbol": "BTC-USDT",
                "best_bid": 1.2,
                "best_ask": 2.2,
                "trade_direction": 255,
                **{f"bid_price_{i}": 1.2 for i in range(1, 11)},
                **{f"bid_size_{i}": 1.0 for i in range(1, 11)},
                **{f"ask_price_{i}": 2.2 for i in range(1, 11)},
                **{f"ask_size_{i}": 1.0 for i in range(1, 11)},
            },
            {
                "timestamp_ns": 4,
                "exchange": "COINBASE",
                "symbol": "BTC-USD",
                "best_bid": 1.3,
                "best_ask": 2.3,
                "trade_direction": 255,
                **{f"bid_price_{i}": 1.3 for i in range(1, 11)},
                **{f"bid_size_{i}": 1.0 for i in range(1, 11)},
                **{f"ask_price_{i}": 2.3 for i in range(1, 11)},
                **{f"ask_size_{i}": 1.0 for i in range(1, 11)},
            },
        ]
    )

    monkeypatch.setattr(
        pipeline, "collect_from_core_bridge", lambda n_ticks, interval_ms: bridge_df
    )
    monkeypatch.setattr(
        pipeline,
        "_collect_l5_ticks_rest",
        lambda n_ticks, interval_ms, exchanges, symbol="BTCUSDT": rest_df,
    )

    out = pipeline.collect_l5_ticks(n_ticks=4, interval_ms=1)
    assert len(out) == 4
    assert set(out["exchange"].to_list()) == {"BINANCE", "KRAKEN", "OKX", "COINBASE"}
    assert "trade_direction" in out.columns


@pytest.mark.skipif(pl is None or pipeline is None, reason="polars is not installed")
def test_collect_l5_ticks_rejects_missing_exchange(monkeypatch) -> None:
    assert pl is not None
    bridge_df = pl.DataFrame(
        [
            {
                "timestamp_ns": 1,
                "exchange": "BINANCE",
                "symbol": "BTCUSDT",
                "best_bid": 1.0,
                "best_ask": 2.0,
                **{f"bid_price_{i}": 1.0 for i in range(1, 11)},
                **{f"bid_size_{i}": 1.0 for i in range(1, 11)},
                **{f"ask_price_{i}": 2.0 for i in range(1, 11)},
                **{f"ask_size_{i}": 1.0 for i in range(1, 11)},
            }
        ]
    )

    monkeypatch.setattr(
        pipeline, "collect_from_core_bridge", lambda n_ticks, interval_ms: bridge_df
    )
    with pytest.raises(RuntimeError, match="missing"):
        pipeline.collect_l5_ticks(
            n_ticks=1,
            interval_ms=1,
            exchanges=["BINANCE", "KRAKEN"],
            allow_rest_fallback=False,
            symbol="BTCUSDT",
        )


@pytest.mark.skipif(pl is None or pipeline is None, reason="polars is not installed")
def test_symbol_family_normalises_exchange_symbols() -> None:
    assert pipeline._symbol_family("PI_XBTUSD") == "BTCUSD"
    assert pipeline._symbol_family("BTC-USD") == "BTCUSD"
    assert pipeline._symbol_family("BTCUSDT") == "BTCUSDT"


# ── Symbol mapper delegation tests ────────────────────────────────────────────
# These verify that each helper delegates to SymbolMapper (when available) and
# produces the correct venue-specific format for representative inputs.

_SKIP_NO_PIPELINE = pytest.mark.skipif(
    pl is None or pipeline is None, reason="polars not installed"
)


@_SKIP_NO_PIPELINE
def test_binance_symbol_compact() -> None:
    assert pipeline._binance_symbol("BTCUSDT") == "BTCUSDT"


@_SKIP_NO_PIPELINE
def test_binance_symbol_strips_separator() -> None:
    assert pipeline._binance_symbol("BTC-USDT") == "BTCUSDT"
    assert pipeline._binance_symbol("BTC/USDT") == "BTCUSDT"


@_SKIP_NO_PIPELINE
def test_okx_symbol_formats_with_dash() -> None:
    assert pipeline._okx_symbol("BTCUSDT") == "BTC-USDT"
    assert pipeline._okx_symbol("BTC/USDT") == "BTC-USDT"


@_SKIP_NO_PIPELINE
def test_okx_symbol_usd_pair() -> None:
    assert pipeline._okx_symbol("BTCUSD") == "BTC-USD"


@_SKIP_NO_PIPELINE
def test_coinbase_symbol_maps_usdt_to_usd() -> None:
    # Coinbase spot only has USD-denominated pairs, never USDT.
    assert pipeline._coinbase_symbol("BTCUSDT") == "BTC-USD"
    assert pipeline._coinbase_symbol("BTC-USDT") == "BTC-USD"


@_SKIP_NO_PIPELINE
def test_coinbase_symbol_preserves_usd() -> None:
    assert pipeline._coinbase_symbol("BTCUSD") == "BTC-USD"


@_SKIP_NO_PIPELINE
def test_kraken_symbol_futures_format() -> None:
    # Kraken Futures REST expects PI_XBTUSD style.
    assert pipeline._kraken_symbol("BTCUSDT") == "PI_XBTUSD"
    assert pipeline._kraken_symbol("BTC-USDT") == "PI_XBTUSD"


@_SKIP_NO_PIPELINE
def test_kraken_symbol_non_btc() -> None:
    assert pipeline._kraken_symbol("ETHUSDT") == "PI_ETHUSD"


@pytest.mark.skipif(not _HAS_TC, reason="trading_core extension not built")
def test_venue_symbols_bindings_round_trip() -> None:
    """SymbolMapper Python bindings return correct venue strings."""
    vs = tc.SymbolMapper.map_all("BTCUSDT")
    assert vs.binance == "BTCUSDT"
    assert vs.okx == "BTC-USDT"
    assert vs.coinbase == "BTC-USDT"
    assert vs.kraken_ws == "BTC/USDT"
    assert vs.kraken_rest == "BTC/USDT"


@pytest.mark.skipif(not _HAS_TC, reason="trading_core extension not built")
def test_venue_symbols_for_exchange() -> None:
    vs = tc.SymbolMapper.map_all("BTC-USDT")
    assert vs.for_exchange(tc.Exchange.BINANCE) == "BTCUSDT"
    assert vs.for_exchange(tc.Exchange.OKX) == "BTC-USDT"
    assert vs.for_exchange(tc.Exchange.KRAKEN) == "BTC/USDT"


@pytest.mark.skipif(not _HAS_TC, reason="trading_core extension not built")
def test_map_for_exchange_convenience() -> None:
    assert tc.SymbolMapper.map_for_exchange(tc.Exchange.BINANCE, "BTC/USDT") == "BTCUSDT"
    assert tc.SymbolMapper.map_for_exchange(tc.Exchange.OKX, "BTCUSDT") == "BTC-USDT"
    assert tc.SymbolMapper.map_for_exchange(tc.Exchange.COINBASE, "BTC_USDT") == "BTC-USDT"
    assert tc.SymbolMapper.map_for_exchange(tc.Exchange.KRAKEN, "BTCUSDT") == "BTC/USDT"


@pytest.mark.skipif(not _HAS_TC, reason="trading_core extension not built")
def test_symbol_mapper_empty_raises() -> None:
    with pytest.raises(Exception):
        tc.SymbolMapper.map_all("")
