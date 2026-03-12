"""
Type stubs for the trading_core pybind11 extension.

These stubs give mypy, pyright, and IDEs full type information without
requiring the compiled .so to be present at type-check time.
"""

from __future__ import annotations

from enum import IntEnum
from typing import Callable

import numpy as np


# ── Enums ──────────────────────────────────────────────────────────────────────
# All enums are scoped (C++ enum class).  Access via qualified name only:
#   trading_core.Exchange.BINANCE   ✓
#   trading_core.BINANCE            ✗  (not exported)

class Exchange(IntEnum):
    BINANCE: int
    OKX: int
    COINBASE: int
    KRAKEN: int
    UNKNOWN: int


class Side(IntEnum):
    BID: int
    ASK: int


class Result(IntEnum):
    SUCCESS: int
    ERROR_INVALID_SEQUENCE: int
    ERROR_INVALID_PRICE: int
    ERROR_INVALID_SIZE: int
    ERROR_SEQUENCE_GAP: int
    ERROR_BOOK_CORRUPTED: int
    ERROR_CONNECTION_LOST: int


class KillReason(IntEnum):
    MANUAL: int
    DRAWDOWN: int
    CIRCUIT_BREAKER: int
    HEARTBEAT_MISSED: int
    BOOK_CORRUPTED: int


# ── Structs ────────────────────────────────────────────────────────────────────

class PriceLevel:
    price: float
    size: float

    def __init__(self, price: float = ..., size: float = ...) -> None: ...
    def __eq__(self, other: object) -> bool: ...
    def __repr__(self) -> str: ...


class Delta:
    side: Side
    price: float
    size: float
    sequence: int
    timestamp_exchange_ns: int
    timestamp_local_ns: int

    def __init__(self) -> None: ...
    def __eq__(self, other: object) -> bool: ...
    def __repr__(self) -> str: ...


class Snapshot:
    symbol: str
    exchange: Exchange
    sequence: int
    bids: list[PriceLevel]
    asks: list[PriceLevel]
    timestamp_exchange_ns: int
    timestamp_local_ns: int

    def __init__(self) -> None: ...
    def __repr__(self) -> str: ...


# ── OrderBook ──────────────────────────────────────────────────────────────────

class OrderBook:
    @property
    def symbol(self) -> str: ...
    @property
    def exchange(self) -> Exchange: ...
    @property
    def tick_size(self) -> float: ...
    @property
    def max_levels(self) -> int: ...
    @property
    def base_price(self) -> float: ...

    def __init__(
        self,
        symbol: str,
        exchange: Exchange,
        tick_size: float = 1.0,
        max_levels: int = 20000,
    ) -> None: ...

    def apply_snapshot(self, snapshot: Snapshot) -> Result: ...
    def apply_delta(self, delta: Delta) -> Result: ...

    def get_best_bid(self) -> float: ...
    def get_best_ask(self) -> float: ...
    def get_mid_price(self) -> float: ...
    def get_spread(self) -> float: ...
    def get_sequence(self) -> int: ...
    def is_initialized(self) -> bool: ...

    def get_top_levels(self, n: int) -> tuple[list[PriceLevel], list[PriceLevel]]:
        """Returns (bids, asks) as lists of PriceLevel, best-first."""
        ...

    def get_levels_array(self, n: int) -> tuple[np.ndarray, np.ndarray]:
        """
        Returns (bids, asks) as float64 numpy arrays of shape (n_actual, 2).
        Columns: [price, size].  Best-first.
        Preferred over get_top_levels() in research loops — no per-level
        Python object allocation.
        """
        ...


# ── KillSwitch ─────────────────────────────────────────────────────────────────

class KillSwitch:
    DEFAULT_HEARTBEAT_TIMEOUT_NS: int

    def __init__(self, heartbeat_timeout_ns: int = ...) -> None: ...
    def is_active(self) -> bool: ...
    def trigger(self, reason: KillReason) -> None: ...
    def reset(self) -> None: ...
    def heartbeat(self) -> None: ...
    def check_heartbeat(self) -> bool: ...
    def get_reason(self) -> KillReason: ...
    @staticmethod
    def reason_to_string(reason: KillReason) -> str: ...


# ── BinanceFeedHandler ─────────────────────────────────────────────────────────

class BinanceFeedHandler:
    def __init__(
        self,
        symbol: str,
        api_key: str = "",
        api_secret: str = "",
        api_url: str = "https://api.binance.com",
        ws_url: str = "wss://stream.binance.com:9443/ws",
    ) -> None: ...

    def set_snapshot_callback(self, callback: Callable[[Snapshot], None]) -> None: ...
    def set_delta_callback(self, callback: Callable[[Delta], None]) -> None: ...
    def set_error_callback(self, callback: Callable[[str], None]) -> None: ...

    def start(self) -> Result:
        """
        Connect and sync the order book.  BLOCKS up to 30 s.
        Releases the GIL while waiting so Python callbacks can fire.
        Returns Result.SUCCESS or Result.ERROR_CONNECTION_LOST.
        """
        ...

    def stop(self) -> None: ...
    def is_running(self) -> bool: ...
    def get_sequence(self) -> int: ...
    def process_message(self, message: str) -> Result: ...

    def __enter__(self) -> BinanceFeedHandler: ...
    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc_val: BaseException | None,
        exc_tb: object,
    ) -> bool: ...


# ── KrakenFeedHandler ──────────────────────────────────────────────────────────

class KrakenFeedHandler:
    def __init__(
        self,
        symbol: str,
        api_key: str = "",
        api_secret: str = "",
        api_url: str = "https://api.kraken.com",
        ws_url: str = "wss://ws.kraken.com/v2",
    ) -> None: ...

    def set_snapshot_callback(self, callback: Callable[[Snapshot], None]) -> None: ...
    def set_delta_callback(self, callback: Callable[[Delta], None]) -> None: ...
    def set_error_callback(self, callback: Callable[[str], None]) -> None: ...

    def start(self) -> Result:
        """
        Connect and sync the order book.  BLOCKS up to 30 s.
        Releases the GIL while waiting so Python callbacks can fire.
        Returns Result.SUCCESS or Result.ERROR_CONNECTION_LOST.
        """
        ...

    def stop(self) -> None: ...
    def is_running(self) -> bool: ...
    def get_sequence(self) -> int: ...
    def process_message(self, message: str) -> Result: ...

    def __enter__(self) -> KrakenFeedHandler: ...
    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc_val: BaseException | None,
        exc_tb: object,
    ) -> bool: ...
