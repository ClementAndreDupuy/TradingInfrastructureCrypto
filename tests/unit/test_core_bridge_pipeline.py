from __future__ import annotations

import mmap
import struct
import tempfile
from pathlib import Path

import pytest

ROOT = Path(__file__).parent.parent.parent
import sys
sys.path.insert(0, str(ROOT))

from research.neural_alpha.core_bridge import CoreBridge

try:
    import polars as pl
except ModuleNotFoundError:  # pragma: no cover - environment-dependent
    pl = None

if pl is not None:
    from research.neural_alpha import pipeline
else:
    pipeline = None

HEADER_FMT = "<8sIIIIQ32s"
SLOT_FMT = "<B15sqd" + "d" * 20 + "64s"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
SLOT_SIZE = struct.calcsize(SLOT_FMT)
CAPACITY = 8


def _write_ring(path: Path, write_seq: int, slots: dict[int, tuple[int, str, int, float]]) -> None:
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

        for seq, (exchange_id, symbol, ts_ns, mid) in slots.items():
            offset = HEADER_SIZE + (seq % CAPACITY) * SLOT_SIZE
            symbol_bytes = symbol.encode("utf-8")[:15].ljust(15, b"\x00")
            bids = [mid - 1 - i * 0.1 for i in range(5)]
            bid_sizes = [1.0 + i for i in range(5)]
            asks = [mid + 1 + i * 0.1 for i in range(5)]
            ask_sizes = [1.5 + i for i in range(5)]
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
                b"\x00" * 64,
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
            8: (0, "BTCUSDT", 1_000, 100.0),
            9: (3, "XBTUSD", 2_000, 101.0),
            10: (1, "BTC-USDT", 3_000, 102.0),
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
        assert "bid_price_5" in rows[-1]
        assert "ask_size_5" in rows[-1]


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
                **{f"bid_price_{i}": 1.0 for i in range(1, 6)},
                **{f"bid_size_{i}": 1.0 for i in range(1, 6)},
                **{f"ask_price_{i}": 2.0 for i in range(1, 6)},
                **{f"ask_size_{i}": 1.0 for i in range(1, 6)},
            },
            {
                "timestamp_ns": 2,
                "exchange": "KRAKEN",
                "symbol": "XBTUSD",
                "best_bid": 1.1,
                "best_ask": 2.1,
                **{f"bid_price_{i}": 1.1 for i in range(1, 6)},
                **{f"bid_size_{i}": 1.0 for i in range(1, 6)},
                **{f"ask_price_{i}": 2.1 for i in range(1, 6)},
                **{f"ask_size_{i}": 1.0 for i in range(1, 6)},
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
                **{f"bid_price_{i}": 1.2 for i in range(1, 6)},
                **{f"bid_size_{i}": 1.0 for i in range(1, 6)},
                **{f"ask_price_{i}": 2.2 for i in range(1, 6)},
                **{f"ask_size_{i}": 1.0 for i in range(1, 6)},
            },
            {
                "timestamp_ns": 4,
                "exchange": "COINBASE",
                "symbol": "BTC-USD",
                "best_bid": 1.3,
                "best_ask": 2.3,
                **{f"bid_price_{i}": 1.3 for i in range(1, 6)},
                **{f"bid_size_{i}": 1.0 for i in range(1, 6)},
                **{f"ask_price_{i}": 2.3 for i in range(1, 6)},
                **{f"ask_size_{i}": 1.0 for i in range(1, 6)},
            },
        ]
    )

    monkeypatch.setattr(pipeline, "collect_from_core_bridge", lambda n_ticks, interval_ms: bridge_df)
    monkeypatch.setattr(pipeline, "_collect_l5_ticks_rest", lambda n_ticks, interval_ms, exchanges: rest_df)

    out = pipeline.collect_l5_ticks(n_ticks=4, interval_ms=1)
    assert len(out) == 4
    assert set(out["exchange"].to_list()) == {"BINANCE", "KRAKEN", "OKX", "COINBASE"}
