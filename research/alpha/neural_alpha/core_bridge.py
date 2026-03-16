"""
CoreBridge — reads live multi-exchange L5 LOB snapshots from the C++ core
via the memory-mapped ring buffer written by core/ipc/lob_publisher.hpp.

File layout of /tmp/trt_lob_feed.bin
──────────────────────────────────────
Header (32 bytes):
  offset  0 : uint64  magic     — 0x31304F424C545254 ("TRTLOB01")
  offset  8 : uint32  version
  offset 12 : uint32  capacity  — number of slots in ring (10 000)
  offset 16 : uint32  slot_size — 256
  offset 20 : uint8[4] _pad
  offset 24 : uint64  write_seq — monotonically increasing (atomic release store)

Ring buffer: capacity × 256-byte slots at offset 32.

Each slot (256 bytes):
  offset  0 : uint8    exchange_id  0=BINANCE 1=KRAKEN 2=OKX 3=COINBASE
  offset  1 : char[15] symbol (null-terminated)
  offset 16 : int64    timestamp_ns
  offset 24 : double   mid_price
  offset 32 : double[5] bid_price
  offset 72 : double[5] bid_size
  offset 112: double[5] ask_price
  offset 152: double[5] ask_size
  offset 192: uint8[64] _pad

Usage
─────
    bridge = CoreBridge()
    if bridge.open():                        # live core is running
        ticks = bridge.read_new_ticks()      # list[dict], newest last
    else:
        ticks = []                           # caller falls back to REST

The returned dicts match the schema produced by pipeline._fetch_*_l5():
    timestamp_ns, exchange, symbol,
    best_bid, best_ask,
    bid_price_{1..5}, bid_size_{1..5},
    ask_price_{1..5}, ask_size_{1..5}
"""
from __future__ import annotations

import mmap
import os
import struct
from typing import Optional

_LOB_FEED_PATH   = "/tmp/trt_lob_feed.bin"
_MAGIC           = 0x31304F424C545254          # "TRTLOB01" LE
_HEADER_SIZE     = 32
_SLOT_SIZE       = 256

# Header field offsets
_HDR_MAGIC_OFF    = 0
_HDR_CAPACITY_OFF = 12
_HDR_WRITE_SEQ_OFF = 24

# Slot field offsets
_SLOT_EXCHANGE_OFF  = 0
_SLOT_SYMBOL_OFF    = 1
_SLOT_TS_NS_OFF     = 16
_SLOT_MID_OFF       = 24
_SLOT_BID_PRICE_OFF = 32
_SLOT_BID_SIZE_OFF  = 72
_SLOT_ASK_PRICE_OFF = 112
_SLOT_ASK_SIZE_OFF  = 152

# struct format: B=uint8 15s=char[15] q=int64 then 21×double then 64s padding
# Sizes: 1+15+8 + 21*8 + 64 = 24 + 168 + 64 = 256 ✓
_SLOT_FMT = "<B15sq" + "d" * 21 + "64s"

_EXCHANGE_LABELS: dict[int, str] = {
    0: "BINANCE",
    1: "KRAKEN",
    2: "OKX",
    3: "COINBASE",
}


class CoreBridge:
    """
    Memory-mapped reader for the C++ LOB ring buffer.

    Thread safety: not thread-safe; use one instance per thread or protect
    with an external lock.
    """

    def __init__(self, path: str = _LOB_FEED_PATH) -> None:
        self._path      = path
        self._mm: Optional[mmap.mmap] = None
        self._last_seq  = 0
        self._capacity  = 0

    # ── Lifecycle ─────────────────────────────────────────────────────────────

    def open(self) -> bool:
        """
        Map the ring-buffer file.  Returns True if the file exists and has the
        correct magic; False otherwise (caller should fall back to REST).
        """
        if not os.path.exists(self._path):
            return False
        try:
            size = os.path.getsize(self._path)
            if size < _HEADER_SIZE:
                return False
            f = open(self._path, "rb")
            self._mm = mmap.mmap(f.fileno(), size, access=mmap.ACCESS_READ)
            f.close()

            magic, = struct.unpack_from("<Q", self._mm, _HDR_MAGIC_OFF)
            if magic != _MAGIC:
                self._mm.close()
                self._mm = None
                return False

            self._capacity, = struct.unpack_from("<I", self._mm, _HDR_CAPACITY_OFF)
            if self._capacity == 0:
                self._mm.close()
                self._mm = None
                return False

            # Seed last_seq to the current write position so we only deliver
            # ticks that arrive *after* this call (no history replay on startup).
            self._last_seq = self._read_write_seq()
            return True
        except Exception:
            if self._mm:
                self._mm.close()
                self._mm = None
            return False

    def close(self) -> None:
        if self._mm:
            self._mm.close()
            self._mm = None

    def is_open(self) -> bool:
        return self._mm is not None

    # ── Reading ───────────────────────────────────────────────────────────────

    def read_new_ticks(self) -> list[dict]:
        """
        Return all slots written since the last call, in order (oldest first).
        Silently drops slots that have been overwritten (ring overflow).
        Returns [] if the bridge is not open or there are no new ticks.
        """
        if self._mm is None:
            return []

        write_seq = self._read_write_seq()
        if write_seq <= self._last_seq:
            return []

        # Avoid reading more than a full ring — drop overwritten slots.
        start_seq = max(self._last_seq, write_seq - self._capacity + 1)

        ticks: list[dict] = []
        for seq in range(start_seq, write_seq):
            slot_idx = seq % self._capacity
            offset   = _HEADER_SIZE + slot_idx * _SLOT_SIZE
            raw      = bytes(self._mm[offset : offset + _SLOT_SIZE])
            tick     = _parse_slot(raw)
            if tick is not None:
                ticks.append(tick)

        self._last_seq = write_seq
        return ticks

    # ── Internals ─────────────────────────────────────────────────────────────

    def _read_write_seq(self) -> int:
        seq, = struct.unpack_from("<Q", self._mm, _HDR_WRITE_SEQ_OFF)
        return seq


# ── Slot parser (module-level for speed) ──────────────────────────────────────

def _parse_slot(raw: bytes) -> dict | None:
    """
    Unpack a 256-byte slot into a dict matching the pipeline's LOB schema.
    Returns None for uninitialised slots (timestamp_ns == 0 or mid == 0.0).
    """
    unpacked = struct.unpack(_SLOT_FMT, raw)
    exchange_id  = unpacked[0]                                # uint8
    symbol_bytes = unpacked[1]                                # bytes[15]
    timestamp_ns = unpacked[2]                                # int64
    mid_price    = unpacked[3]                                # double
    # Doubles at indices 4..24 (21 total):
    #   4..8   bid_price[5]
    #   9..13  bid_size[5]
    #   14..18 ask_price[5]
    #   19..23 ask_size[5]

    if timestamp_ns == 0 or mid_price == 0.0:
        return None

    symbol   = symbol_bytes.rstrip(b"\x00").decode("ascii", errors="replace")
    exchange = _EXCHANGE_LABELS.get(exchange_id, f"UNK_{exchange_id}")

    bid_prices = list(unpacked[4:9])
    bid_sizes  = list(unpacked[9:14])
    ask_prices = list(unpacked[14:19])
    ask_sizes  = list(unpacked[19:24])

    row: dict = {
        "timestamp_ns": timestamp_ns,
        "exchange":     exchange,
        "symbol":       symbol,
        "best_bid":     bid_prices[0],
        "best_ask":     ask_prices[0],
    }
    for i in range(5):
        row[f"bid_price_{i + 1}"] = bid_prices[i]
        row[f"bid_size_{i + 1}"]  = bid_sizes[i]
        row[f"ask_price_{i + 1}"] = ask_prices[i]
        row[f"ask_size_{i + 1}"]  = ask_sizes[i]

    return row
