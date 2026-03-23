from __future__ import annotations

from dataclasses import dataclass
from typing import Any

try:
    import trading_core as _tc
except ImportError:
    _tc = None


@dataclass(frozen=True)
class VenueSymbolsFallback:
    binance: str
    okx: str
    coinbase: str
    kraken_rest: str


def parse_symbol_parts(symbol: str) -> tuple[str, str]:
    clean_symbol = symbol.strip().upper()
    if not clean_symbol:
        raise ValueError("symbol must not be empty")
    for separator in ("-", "/", "_"):
        if separator in clean_symbol:
            base, quote = clean_symbol.split(separator, maxsplit=1)
            if base and quote:
                return base, quote
    for suffix in (
        "FDUSD",
        "USDT",
        "USDC",
        "USDK",
        "TUSD",
        "BUSD",
        "DAI",
        "USD",
        "EUR",
        "GBP",
        "JPY",
        "TRY",
        "AUD",
        "CAD",
        "CHF",
        "BTC",
        "ETH",
    ):
        if clean_symbol.endswith(suffix) and len(clean_symbol) > len(suffix):
            return clean_symbol[: -len(suffix)], suffix
    return clean_symbol[:-3], clean_symbol[-3:]


def map_venue_symbols(symbol: str) -> Any:
    if _tc is not None:
        return _tc.SymbolMapper.map_all(symbol)
    base, quote = parse_symbol_parts(symbol)
    return VenueSymbolsFallback(
        binance=f"{base}{quote}",
        okx=f"{base}-{quote}",
        coinbase=f"{base}-{quote}",
        kraken_rest=f"{base}/{quote}",
    )


def binance_symbol(symbol: str) -> str:
    return map_venue_symbols(symbol).binance


def kraken_symbol(symbol: str) -> str:
    return map_venue_symbols(symbol).kraken_rest


def okx_symbol(symbol: str) -> str:
    return map_venue_symbols(symbol).okx


def coinbase_symbol(symbol: str) -> str:
    return map_venue_symbols(symbol).coinbase


def symbol_family(symbol: str) -> str:
    family = symbol.upper()
    if family.startswith("PI_"):
        family = family[3:]
    if family.endswith("PERP"):
        family = family[:-4]
    family = family.replace("XBT", "BTC")
    return map_venue_symbols(family).binance.replace("/", "")
