from __future__ import annotations

from enum import IntEnum
from typing import Callable, TypeVar

import numpy as np

__version__: str

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


# ── SymbolMapper ───────────────────────────────────────────────────────────────


class VenueSymbols:
    """Venue-specific symbol strings produced by SymbolMapper.map_all().

    All fields are read-only strings in the format expected by each exchange.
    """

    binance: str  # e.g. "BTCUSDT"
    okx: str  # e.g. "BTC-USDT"
    coinbase: str  # e.g. "BTC-USDT"
    kraken_ws: str  # e.g. "BTC/USDT"
    kraken_rest: str  # e.g. "BTC/USDT"

    def for_exchange(self, exchange: Exchange) -> str:
        """Return the venue-specific symbol string for the given Exchange."""
        ...

    def __repr__(self) -> str: ...


class SymbolMapper:
    """Converts a canonical symbol string to venue-specific formats.

    Handles input with separators (``-``, ``/``, ``_``) or no separator
    (e.g. ``"BTCUSDT"``).  All methods are static; do not instantiate.

    Raises ``ValueError`` when the symbol is empty.
    """

    @staticmethod
    def map_all(symbol: str) -> VenueSymbols:
        """Map a canonical symbol to all venue formats.

        Returns a :class:`VenueSymbols` containing exchange-specific strings.
        Raises ``ValueError`` on empty input.
        """
        ...

    @staticmethod
    def map_for_exchange(exchange: Exchange, symbol: str) -> str:
        """Convenience: map a canonical symbol to a single venue's format."""
        ...


# ── Structs ────────────────────────────────────────────────────────────────────


class PriceLevel:
    """A single price/size level on one side of the order book.

    Fields default to 0.0 on default construction.
    Equality is defined by exact float comparison of both fields.
    """

    price: float  # Absolute price in quote currency (e.g. USD)
    size: float  # Quantity at this price level (in base currency)

    def __init__(self, price: float = ..., size: float = ...) -> None: ...

    def __eq__(self, other: object) -> bool: ...

    def __repr__(self) -> str: ...


class Delta:
    """A single incremental order book update.

    Produced by the feed handler's delta callback.  All integer fields
    default to 0; side defaults to Side.BID on default construction.
    Timestamps are nanoseconds since Unix epoch (PTP-synced where available).
    """

    side: Side  # Which side of the book is updated
    price: float  # Price level being updated
    size: float  # New size at this level (0.0 = remove level)
    sequence: int  # Exchange sequence number for gap detection
    timestamp_exchange_ns: int  # Exchange-reported event time (ns since epoch)
    timestamp_local_ns: int  # Local receipt timestamp (ns since epoch)

    def __init__(self) -> None: ...

    def __eq__(self, other: object) -> bool: ...

    def __repr__(self) -> str: ...


class Snapshot:
    """A full order book snapshot.

    Produced by the feed handler's snapshot callback after initial sync.
    Bids and asks are ordered best-first (descending bids, ascending asks).
    Timestamps are nanoseconds since Unix epoch (PTP-synced where available).
    """

    symbol: str  # Trading pair (e.g. "BTCUSDT", "XBT/USD")
    exchange: Exchange  # Source exchange
    sequence: int  # Snapshot sequence number
    bids: list[PriceLevel]  # Bid levels, best (highest) first
    asks: list[PriceLevel]  # Ask levels, best (lowest) first
    checksum: int  # Optional exchange-provided checksum
    checksum_present: bool  # True when checksum field is valid
    timestamp_exchange_ns: int  # Exchange-reported timestamp (ns since epoch)
    timestamp_local_ns: int  # Local receipt timestamp (ns since epoch)

    def __init__(self) -> None: ...

    def __repr__(self) -> str: ...


# ── OrderBook ──────────────────────────────────────────────────────────────────


class OrderBook:
    """Flat-array order book indexed on a price-tick grid.

    O(1) apply_delta, O(1) best-bid/ask lookup.  Cache-local: the entire
    price grid fits in a few cache lines for typical tick sizes and depths.

    Must be initialized via apply_snapshot() before apply_delta() will succeed.
    """

    @property
    def symbol(self) -> str:
        """Trading pair symbol passed at construction."""
        ...

    @property
    def exchange(self) -> Exchange:
        """Exchange passed at construction."""
        ...

    @property
    def tick_size(self) -> float:
        """Minimum price increment (price grid resolution)."""
        ...

    @property
    def max_levels(self) -> int:
        """Total preallocated capacity of the flat price grid."""
        ...

    @property
    def active_levels(self) -> int:
        """Currently active grid window chosen from the latest snapshot."""
        ...

    @property
    def base_price(self) -> float:
        """Grid anchor price set by the last apply_snapshot()."""
        ...

    def __init__(
            self,
            symbol: str,
            exchange: Exchange,
            tick_size: float = 1.0,
            max_levels: int = 20000,
    ) -> None: ...

    def apply_snapshot(self, snapshot: Snapshot) -> Result:
        """Replace the entire book with a new snapshot, re-centering the grid."""
        ...

    def apply_delta(self, delta: Delta) -> Result:
        """Apply one incremental update.  Returns ERROR_BOOK_CORRUPTED if not initialized."""
        ...

    def get_best_bid(self) -> float: ...

    def get_best_ask(self) -> float: ...

    def get_mid_price(self) -> float: ...

    def get_spread(self) -> float: ...

    def get_sequence(self) -> int: ...

    def is_initialized(self) -> bool: ...

    def get_top_levels(self, n: int) -> tuple[list[PriceLevel], list[PriceLevel]]:
        """Return (bids, asks) as Python lists of PriceLevel, best-first.

        Use get_levels_array() in research loops for better performance.
        """
        ...

    def get_levels_array(self, n: int) -> tuple[np.ndarray, np.ndarray]:
        """Return (bids, asks) as C-contiguous float64 arrays, shape (n_actual, 2).

        Columns: ``[:, 0]`` = price, ``[:, 1]`` = size.  Best-first.
        n_actual <= n (clamped to available depth).

        Preferred over get_top_levels() in backtesting loops — allocates two
        numpy arrays rather than n Python PriceLevel objects.

        Arrays are independent Python objects; modifying them does not affect
        the internal book state.
        """
        ...


# ── KillSwitch ─────────────────────────────────────────────────────────────────


class KillSwitch:
    """Hardware-level kill switch with heartbeat monitoring.

    Once triggered, is_active() returns True permanently until reset() is
    called by an operator.  All methods are thread-safe and sub-microsecond.
    """

    DEFAULT_HEARTBEAT_TIMEOUT_NS: int  # Default: 5 000 000 000 ns (5 s)

    def __init__(self, heartbeat_timeout_ns: int = ...) -> None: ...

    def is_active(self) -> bool:
        """Hot-path check (< 1 µs).  Returns True if kill switch is armed."""
        ...

    def trigger(self, reason: KillReason) -> None:
        """Arm the kill switch.  Idempotent — first reason wins."""
        ...

    def reset(self) -> None:
        """Disarm.  Call only after manual operator review and all orders confirmed canceled."""
        ...

    def heartbeat(self) -> None:
        """Must be called from the hot-path loop at < heartbeat_timeout_ns intervals."""
        ...

    def check_heartbeat(self) -> bool:
        """Call from monitoring thread.  Returns False and triggers kill if heartbeat stalled."""
        ...

    def get_reason(self) -> KillReason: ...

    @staticmethod
    def reason_to_string(reason: KillReason) -> str: ...



_FH = TypeVar("_FH", bound="_FeedHandlerBase")


class _FeedHandlerBase:
    """Common interface shared by all exchange feed handlers.

    Thread model
    ------------
    start() spawns a background WebSocket thread and blocks up to 30 s
    waiting for STREAMING state.  The GIL is released for the entire
    duration of start() and stop(), allowing Python threads to run.

    Callbacks are invoked from the background WebSocket thread with the GIL
    held.  Exceptions raised by Python callbacks are caught, logged to stderr,
    and do NOT propagate — they will not crash the C++ thread.

    Lifetime
    --------
    The handler MUST outlive all registered callbacks.  Do not drop the last
    Python reference while is_running() is True; call stop() first.
    """

    def set_snapshot_callback(self, callback: Callable[[Snapshot], None]) -> None:
        """Register callback invoked on each full book snapshot.

        The handler holds a strong reference to ``callback`` for its lifetime.
        Exceptions raised by the callback are logged but not re-raised.
        """
        ...

    def set_delta_callback(self, callback: Callable[[Delta], None]) -> None:
        """Register callback invoked on each incremental book update."""
        ...

    def set_error_callback(self, callback: Callable[[str], None]) -> None:
        """Register callback invoked on WebSocket errors.  Argument is an error string."""
        ...

    def start(self) -> Result:
        """Connect and sync the order book.

        BLOCKS up to 30 s.  Releases the GIL while waiting.
        Returns Result.SUCCESS or Result.ERROR_CONNECTION_LOST.
        """
        ...

    def stop(self) -> None:
        """Disconnect and join the background WebSocket thread.  Releases the GIL."""
        ...

    def is_running(self) -> bool:
        """True if the background WebSocket thread is active.

        Note: is_running() and get_sequence() are independent atomic reads
        with no consistency guarantee between them.
        """
        ...

    def get_sequence(self) -> int:
        """Most recently processed sequence number.  Atomic read."""
        ...

    @property
    def tick_size(self) -> float:
        """Price tick size fetched from the exchange symbol-info endpoint during start().

        Returns 0.0 before start() is called or if the REST call failed.
        Pass this value as tick_size when constructing the OrderBook so the grid
        precision matches the exchange exactly.
        """
        ...

    def process_message(self, message: str) -> Result:
        """TESTING ONLY — inject a raw WebSocket JSON message directly.

        Bypasses the normal feed state machine.  Do not call in production;
        fabricated messages will corrupt the order book state.
        """
        ...

    def __enter__(self: _FH) -> _FH: ...

    def __exit__(
            self,
            exc_type: type[BaseException] | None,
            exc_val: BaseException | None,
            exc_tb: object,
    ) -> bool: ...


# ── Concrete Feed Handlers ─────────────────────────────────────────────────────


class BinanceFeedHandler(_FeedHandlerBase):
    """Binance spot order book feed handler using the diff-depth WebSocket stream."""

    def __init__(
            self,
            symbol: str,
            api_url: str = "https://api.binance.com",
            ws_url: str = "wss://stream.binance.com:9443/ws",
    ) -> None: ...


class KrakenFeedHandler(_FeedHandlerBase):
    """Kraken order book feed handler using the WebSocket v2 book channel."""
    def __init__(
            self,
            symbol: str,
            api_key: str = "",
            api_secret: str = "",
            api_url: str = "https://api.kraken.com",
            ws_url: str = "wss://ws.kraken.com/v2",
    ) -> None: ...


class OkxFeedHandler(_FeedHandlerBase):
    """OKX order book feed handler using the WebSocket v5 public channel."""
    def __init__(
            self,
            symbol: str,
            api_url: str = "https://www.okx.com",
            ws_url: str = "wss://ws.okx.com:8443/ws/v5/public",
    ) -> None: ...


class CoinbaseFeedHandler(_FeedHandlerBase):
    """Coinbase Advanced Trade order book feed handler using the WebSocket channel."""
    def __init__(
            self,
            symbol: str,
            ws_url: str = "wss://advanced-trade-ws.coinbase.com",
            api_url: str = "https://api.exchange.coinbase.com",
    ) -> None: ...
