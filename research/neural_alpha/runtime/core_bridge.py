from __future__ import annotations
import mmap
import os
import struct
from typing import Any

RING_PATH = "/tmp/trt_ipc/trt_lob_feed.bin"
HEADER_FMT = "<8sIIIIQ32s"
SLOT_FMT = "<B15sqd" + "d" * 40 + "I" * 20 + "B7s"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
SLOT_SIZE = struct.calcsize(SLOT_FMT)
EXPECTED_MAGIC = b"TRTLOB01"
_EXCHANGE_MAP = {0: "BINANCE", 1: "OKX", 2: "COINBASE", 3: "KRAKEN"}


class CoreBridge:

    def __init__(self, path: str = RING_PATH) -> None:
        self.path = path
        self._fd: int | None = None
        self._mm: mmap.mmap | None = None
        self._capacity = 0
        self._last_seq = 0

    def open(self) -> bool:
        if self._mm is not None:
            return True
        if not os.path.exists(self.path):
            return False
        try:
            fd = os.open(self.path, os.O_RDONLY)
            mm = mmap.mmap(fd, 0, access=mmap.ACCESS_READ)
            (magic, _, slot_size, capacity, _, write_seq, _) = struct.unpack_from(HEADER_FMT, mm, 0)
            if magic != EXPECTED_MAGIC or slot_size != SLOT_SIZE or capacity <= 0:
                mm.close()
                os.close(fd)
                return False
            self._fd = fd
            self._mm = mm
            self._capacity = capacity
            self._last_seq = write_seq
            return True
        except OSError:
            return False

    def close(self) -> None:
        if self._mm is not None:
            self._mm.close()
            self._mm = None
        if self._fd is not None:
            os.close(self._fd)
            self._fd = None
        self._capacity = 0
        self._last_seq = 0

    def read_new_ticks(self) -> list[dict[str, Any]]:
        if self._mm is None and (not self.open()):
            return []
        assert self._mm is not None
        (magic, _, slot_size, capacity, _, write_seq, _) = struct.unpack_from(
            HEADER_FMT, self._mm, 0
        )
        if magic != EXPECTED_MAGIC or slot_size != SLOT_SIZE or capacity <= 0:
            return []
        start_seq = self._last_seq
        min_valid_seq = max(0, write_seq - capacity)
        if start_seq < min_valid_seq:
            start_seq = min_valid_seq
        rows: list[dict[str, Any]] = []
        for seq in range(start_seq, write_seq):
            slot_offset = HEADER_SIZE + seq % capacity * SLOT_SIZE
            unpacked = struct.unpack_from(SLOT_FMT, self._mm, slot_offset)
            exchange_id = unpacked[0]
            symbol_raw = unpacked[1]
            timestamp_ns = unpacked[2]
            price_size_values = unpacked[4:44]
            bids_p = price_size_values[0:10]
            bids_s = price_size_values[10:20]
            asks_p = price_size_values[20:30]
            asks_s = price_size_values[30:40]
            bid_ocs = unpacked[44:54]
            ask_ocs = unpacked[54:64]
            trade_direction = unpacked[64]
            best_bid = float(bids_p[0])
            best_ask = float(asks_p[0])
            if best_bid <= 0.0 or best_ask <= 0.0 or best_ask <= best_bid:
                continue
            symbol = symbol_raw.split(b"\x00", 1)[0].decode("utf-8", errors="ignore")
            exchange = _EXCHANGE_MAP.get(exchange_id, "UNKNOWN")
            row: dict[str, Any] = {
                "timestamp_ns": int(timestamp_ns),
                "exchange": exchange,
                "symbol": symbol,
                "best_bid": best_bid,
                "best_ask": best_ask,
                "trade_direction": int(trade_direction),
            }
            for i in range(10):
                row[f"bid_price_{i + 1}"] = float(bids_p[i])
                row[f"bid_size_{i + 1}"] = float(bids_s[i])
                row[f"ask_price_{i + 1}"] = float(asks_p[i])
                row[f"ask_size_{i + 1}"] = float(asks_s[i])
                row[f"bid_oc_{i + 1}"] = int(bid_ocs[i])
                row[f"ask_oc_{i + 1}"] = int(ask_ocs[i])
            rows.append(row)
        self._last_seq = write_seq
        return rows
