"""
Perpetual Futures Cross-Exchange Arbitrage Backtest
====================================================

Event-driven replay of L2 tick data for the Binance BTCUSDT perp /
Kraken PI_XBTUSD futures pair.

Strategy modes replicated from core/strategy/perp_arb_strategy.hpp:
  TAKER  : Cross both sides as IOC when net spread > taker_threshold_bps
  MM     : Post limit orders inside the book; hedge on fill

Fee model (configurable, defaults match live config):
  Binance perp  : maker 2 bps, taker 5 bps
  Kraken futures: maker 2 bps, taker 5 bps

Input data schema (Parquet or CSV, one row per book snapshot):
  timestamp_ns   : int64   — nanoseconds since epoch
  exchange       : str     — "BINANCE" or "KRAKEN"
  best_bid       : float64
  best_ask       : float64
  bid_size_1     : float64 — top-of-book size
  ask_size_1     : float64

Output:
  - Trade log (Polars DataFrame)
  - Equity curve (cumulative P&L)
  - Performance metrics: Sharpe, max drawdown, win rate, avg hold time

Usage:
  python -m research.backtest.perp_arb_backtest \\
      --binance data/binance_perp_btc_ticks.parquet \\
      --kraken  data/kraken_futures_btc_ticks.parquet \\
      --qty 0.001 \\
      --taker-bps 12.0 \\
      --mm-bps 6.0
"""

from __future__ import annotations

import argparse
import math
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import numpy as np
import polars as pl


# ── Configuration ─────────────────────────────────────────────────────────────


@dataclass
class BacktestConfig:
    trade_qty: float = 0.001                # BTC per leg
    taker_threshold_bps: float = 12.0       # Min spread to cross (taker)
    mm_spread_target_bps: float = 6.0       # Min spread to post quotes (maker)
    quote_offset_bps: float = 1.0           # Offset from best when quoting
    binance_maker_fee_bps: float = 2.0
    binance_taker_fee_bps: float = 5.0
    kraken_maker_fee_bps: float = 2.0
    kraken_taker_fee_bps: float = 5.0
    max_position_btc: float = 0.05          # Max net position per exchange
    max_drawdown_usd: float = -500.0        # Stop trading below this
    quote_ttl_ns: int = 500_000_000         # 500 ms quote TTL
    hedge_timeout_ns: int = 2_000_000_000   # 2 s forced hedge timeout
    latency_ns: int = 5_000_000             # Simulated 5 ms one-way latency


# ── Book state (one per exchange) ────────────────────────────────────────────


@dataclass
class BookSnapshot:
    ts_ns: int = 0
    best_bid: float = 0.0
    best_ask: float = 0.0
    bid_size: float = 0.0
    ask_size: float = 0.0

    @property
    def mid(self) -> float:
        return (self.best_bid + self.best_ask) / 2.0 if self.best_bid and self.best_ask else 0.0

    @property
    def spread_bps(self) -> float:
        if not self.best_bid or not self.best_ask:
            return 0.0
        return (self.best_ask - self.best_bid) / self.best_bid * 10_000.0

    def is_valid(self) -> bool:
        return self.best_bid > 0 and self.best_ask > 0 and self.best_ask > self.best_bid


# ── Virtual order ─────────────────────────────────────────────────────────────


@dataclass
class VirtualOrder:
    oid: int
    exchange: str          # "BINANCE" | "KRAKEN"
    side: str              # "BID" | "ASK"
    order_type: str        # "LIMIT" | "MARKET" | "IOC"
    price: float
    qty: float
    submit_ns: int
    is_maker: bool = False
    filled: bool = False
    fill_price: float = 0.0
    fill_ns: int = 0


# ── Trade record ──────────────────────────────────────────────────────────────


@dataclass
class TradeRecord:
    open_ns: int
    close_ns: int
    buy_exchange: str
    sell_exchange: str
    buy_price: float
    sell_price: float
    qty: float
    buy_fee_bps: float
    sell_fee_bps: float
    mode: str          # "TAKER" | "MM"

    @property
    def gross_pnl(self) -> float:
        return (self.sell_price - self.buy_price) * self.qty

    @property
    def total_fee(self) -> float:
        mid = (self.buy_price + self.sell_price) / 2.0
        return ((self.buy_fee_bps + self.sell_fee_bps) / 10_000.0) * mid * self.qty

    @property
    def net_pnl(self) -> float:
        return self.gross_pnl - self.total_fee

    @property
    def hold_time_ms(self) -> float:
        return (self.close_ns - self.open_ns) / 1_000_000.0


# ── Backtest engine ───────────────────────────────────────────────────────────


class PerpArbBacktest:
    """
    Event-driven perpetual futures arbitrage backtest.

    Replays tick data in chronological order. Each tick represents
    a book update on one exchange; the strategy evaluates both books
    after every tick and emits virtual orders.
    """

    def __init__(self, cfg: BacktestConfig) -> None:
        self.cfg = cfg
        self._oid = 0
        self._trades: list[TradeRecord] = []
        self._orders: list[VirtualOrder] = []
        self._pnl = 0.0
        self._peak_pnl = 0.0
        self._max_drawdown = 0.0

        # Exchange books
        self._bin: BookSnapshot = BookSnapshot()
        self._kra: BookSnapshot = BookSnapshot()

        # Position tracking (net, positive = long)
        self._pos: dict[str, float] = {"BINANCE": 0.0, "KRAKEN": 0.0}

        # Pending taker leg tracking
        self._taker_buy_oid: Optional[int] = None
        self._taker_sell_oid: Optional[int] = None
        self._taker_open_ns: int = 0
        self._taker_buy_px: float = 0.0
        self._taker_buy_ex: str = ""
        self._taker_sell_ex: str = ""
        self._taker_buy_fee: float = 0.0
        self._taker_sell_fee: float = 0.0

        # MM quote tracking
        self._mm_anchor_oid: Optional[int] = None
        self._mm_hedge_oid: Optional[int] = None
        self._mm_anchor_side: str = ""
        self._mm_anchor_ex: str = ""
        self._mm_anchor_px: float = 0.0
        self._mm_anchor_fee: float = 0.0
        self._mm_open_ns: int = 0
        self._mm_quote_ns: int = 0

    # ── Public API ───────────────────────────────────────────────────────────

    def run(self, ticks: pl.DataFrame) -> dict:
        """
        Run the backtest on a merged, sorted tick DataFrame.

        Args:
            ticks: DataFrame with columns:
                   timestamp_ns, exchange, best_bid, best_ask,
                   bid_size_1, ask_size_1

        Returns:
            dict with keys: trades, equity_curve, metrics
        """
        for row in ticks.iter_rows(named=True):
            self._process_tick(row)

        return {
            "trades":       self._build_trades_df(),
            "equity_curve": self._build_equity_curve(),
            "metrics":      self._compute_metrics(),
        }

    # ── Tick processing ──────────────────────────────────────────────────────

    def _process_tick(self, row: dict) -> None:
        ts  = row["timestamp_ns"]
        ex  = row["exchange"]
        bid = row["best_bid"]
        ask = row["best_ask"]

        # Update appropriate book
        snap = BookSnapshot(
            ts_ns=ts,
            best_bid=bid,
            best_ask=ask,
            bid_size=row.get("bid_size_1", 0.0),
            ask_size=row.get("ask_size_1", 0.0),
        )
        if ex == "BINANCE":
            self._bin = snap
        else:
            self._kra = snap

        # Check pending orders for fills
        self._check_pending_fills(ts)

        # Both books must be valid and fresh (within 1 second of each other)
        if not self._bin.is_valid() or not self._kra.is_valid():
            return
        if abs(self._bin.ts_ns - self._kra.ts_ns) > 1_000_000_000:
            return

        # Stop if kill switch / drawdown breach
        if self._pnl < self.cfg.max_drawdown_usd:
            return

        # Evaluate strategy
        self._evaluate(ts)

    def _evaluate(self, ts: int) -> None:
        if self._taker_buy_oid is not None or self._mm_anchor_oid is not None:
            return  # already in flight

        bin_bid, bin_ask = self._bin.best_bid, self._bin.best_ask
        kra_bid, kra_ask = self._kra.best_bid, self._kra.best_ask

        # Taker spreads (sell_best_bid - buy_best_ask) / buy_best_ask * 1e4
        spread_bin_buy = _spread_bps(bin_ask, kra_bid)   # buy Binance, sell Kraken
        spread_kra_buy = _spread_bps(kra_ask, bin_bid)   # buy Kraken, sell Binance

        if spread_bin_buy >= self.cfg.taker_threshold_bps:
            self._run_taker(ts, bin_ask, kra_bid, "BINANCE", "KRAKEN")
            return
        if spread_kra_buy >= self.cfg.taker_threshold_bps:
            self._run_taker(ts, kra_ask, bin_bid, "KRAKEN", "BINANCE")
            return

        # Market maker
        avg_spread = (self._bin.spread_bps + self._kra.spread_bps) / 2.0
        if avg_spread >= self.cfg.mm_spread_target_bps and self._mm_anchor_oid is None:
            self._run_mm(ts, bin_bid, bin_ask, kra_bid, kra_ask)

    # ── Taker arb ────────────────────────────────────────────────────────────

    def _run_taker(self, ts: int,
                   buy_px: float, sell_px: float,
                   buy_ex: str, sell_ex: str) -> None:
        # Position check
        if abs(self._pos[buy_ex] + self.cfg.trade_qty) > self.cfg.max_position_btc:
            return
        if abs(self._pos[sell_ex] - self.cfg.trade_qty) > self.cfg.max_position_btc:
            return

        exec_ts = ts + self.cfg.latency_ns

        buy_fee  = self._taker_fee(buy_ex)
        sell_fee = self._taker_fee(sell_ex)

        # Net profit check
        net_spread = _spread_bps(buy_px, sell_px) - (buy_fee + sell_fee)
        if net_spread * buy_px * self.cfg.trade_qty / 10_000.0 < 0.05:  # $0.05 min
            return

        buy_order  = self._make_ioc(exec_ts, buy_ex,  "BID", buy_px,  self.cfg.trade_qty)
        sell_order = self._make_ioc(exec_ts, sell_ex, "ASK", sell_px, self.cfg.trade_qty)

        self._taker_buy_oid  = buy_order.oid
        self._taker_sell_oid = sell_order.oid
        self._taker_open_ns  = ts
        self._taker_buy_px   = buy_px
        self._taker_buy_ex   = buy_ex
        self._taker_sell_ex  = sell_ex
        self._taker_buy_fee  = buy_fee
        self._taker_sell_fee = sell_fee

        self._orders.extend([buy_order, sell_order])

    def _check_taker_fills(self, ts: int) -> None:
        if self._taker_buy_oid is None:
            return

        buy_order  = self._find_order(self._taker_buy_oid)
        sell_order = self._find_order(self._taker_sell_oid)
        if not buy_order or not sell_order:
            return

        # Simulate IOC fill: fills if book crossed within latency window
        self._try_ioc_fill(buy_order,  ts, self._get_book(self._taker_buy_ex))
        self._try_ioc_fill(sell_order, ts, self._get_book(self._taker_sell_ex))

        if buy_order.filled and sell_order.filled:
            self._record_taker_trade(buy_order, sell_order)
        elif buy_order.filled and not sell_order.filled:
            # Leg 1 filled, leg 2 missed — force close leg 1 at market
            close_px = self._get_book(self._taker_buy_ex).best_ask
            loss = (buy_order.fill_price - close_px) * buy_order.qty
            self._pnl += loss
        # If neither filled: clean up silently

        self._taker_buy_oid  = None
        self._taker_sell_oid = None

    def _record_taker_trade(self, buy_ord: VirtualOrder, sell_ord: VirtualOrder) -> None:
        trade = TradeRecord(
            open_ns=self._taker_open_ns,
            close_ns=max(buy_ord.fill_ns, sell_ord.fill_ns),
            buy_exchange=self._taker_buy_ex,
            sell_exchange=self._taker_sell_ex,
            buy_price=buy_ord.fill_price,
            sell_price=sell_ord.fill_price,
            qty=self.cfg.trade_qty,
            buy_fee_bps=self._taker_buy_fee,
            sell_fee_bps=self._taker_sell_fee,
            mode="TAKER",
        )
        self._trades.append(trade)
        self._pnl += trade.net_pnl
        self._pos[self._taker_buy_ex]  += self.cfg.trade_qty
        self._pos[self._taker_sell_ex] -= self.cfg.trade_qty
        self._update_drawdown()

    # ── Market maker ─────────────────────────────────────────────────────────

    def _run_mm(self, ts: int,
                bin_bid: float, bin_ask: float,
                kra_bid: float, kra_ask: float) -> None:
        # Post buy on Binance (inside best bid) and sell on Kraken (inside best ask)
        offset_bps = self.cfg.quote_offset_bps / 10_000.0
        anchor_buy_px  = bin_bid * (1.0 + offset_bps)   # improve Binance bid
        anchor_sell_px = kra_ask * (1.0 - offset_bps)   # improve Kraken ask

        # Verify we'd capture positive spread after round-trip maker fees
        mm_fees = self._maker_fee("BINANCE") + self._maker_fee("KRAKEN")
        captured = _spread_bps(anchor_buy_px, anchor_sell_px)
        if captured < mm_fees + self.cfg.mm_spread_target_bps:
            return

        exec_ts = ts + self.cfg.latency_ns

        # Anchor: buy on Binance (resting limit)
        anchor = self._make_limit(exec_ts, "BINANCE", "BID", anchor_buy_px, self.cfg.trade_qty)
        anchor.is_maker = True
        self._mm_anchor_oid  = anchor.oid
        self._mm_anchor_side = "BID"
        self._mm_anchor_ex   = "BINANCE"
        self._mm_anchor_px   = anchor_buy_px
        self._mm_anchor_fee  = self._maker_fee("BINANCE")
        self._mm_open_ns     = ts
        self._mm_quote_ns    = ts

        self._orders.append(anchor)

    def _check_mm_fills(self, ts: int) -> None:
        if self._mm_anchor_oid is None:
            return

        anchor = self._find_order(self._mm_anchor_oid)
        if not anchor:
            return

        # TTL check
        if not anchor.filled and (ts - self._mm_quote_ns) > self.cfg.quote_ttl_ns:
            anchor.filled = True  # cancel (treat as not filled)
            self._mm_anchor_oid = None
            return

        book = self._get_book(anchor.exchange)

        # Limit fill: bid fills when best_ask crosses down to price
        if not anchor.filled and anchor.side == "BID":
            if book.best_ask <= anchor.price and book.best_ask > 0:
                anchor.fill_price = book.best_ask  # filled at ask (passive)
                anchor.fill_ns    = ts
                anchor.filled     = True
                self._pos[anchor.exchange] += anchor.qty

                # Submit hedge immediately: sell on Kraken at market
                hedge_ex  = "KRAKEN"
                hedge_px  = self._kra.best_bid  # sell at bid
                hedge_fee = self._taker_fee(hedge_ex)

                if hedge_px <= 0:
                    self._mm_anchor_oid = None
                    return

                trade = TradeRecord(
                    open_ns=self._mm_open_ns,
                    close_ns=ts + self.cfg.latency_ns,
                    buy_exchange=anchor.exchange,
                    sell_exchange=hedge_ex,
                    buy_price=anchor.fill_price,
                    sell_price=hedge_px,
                    qty=self.cfg.trade_qty,
                    buy_fee_bps=self._mm_anchor_fee,
                    sell_fee_bps=hedge_fee,
                    mode="MM",
                )
                self._trades.append(trade)
                self._pnl += trade.net_pnl
                self._pos[hedge_ex] -= self.cfg.trade_qty
                self._update_drawdown()
                self._mm_anchor_oid = None

    # ── Fill simulation ───────────────────────────────────────────────────────

    def _try_ioc_fill(self, order: VirtualOrder, ts: int, book: BookSnapshot) -> None:
        if order.filled:
            return
        if ts < order.submit_ns:
            return  # latency not elapsed yet

        if order.side == "BID":
            if book.best_ask > 0 and order.price >= book.best_ask:
                order.fill_price = book.best_ask
                order.fill_ns    = ts
                order.filled     = True
        else:
            if book.best_bid > 0 and order.price <= book.best_bid:
                order.fill_price = book.best_bid
                order.fill_ns    = ts
                order.filled     = True

        # IOC: if not filled immediately, cancel (leave as not filled)
        if not order.filled and order.order_type == "IOC":
            order.filled = True
            order.fill_price = 0.0  # sentinel: not filled

    def _check_pending_fills(self, ts: int) -> None:
        self._check_taker_fills(ts)
        self._check_mm_fills(ts)

    # ── Order factory ────────────────────────────────────────────────────────

    def _next_oid(self) -> int:
        self._oid += 1
        return self._oid

    def _make_limit(self, ts: int, ex: str, side: str, price: float, qty: float) -> VirtualOrder:
        return VirtualOrder(self._next_oid(), ex, side, "LIMIT", price, qty, ts)

    def _make_ioc(self, ts: int, ex: str, side: str, price: float, qty: float) -> VirtualOrder:
        return VirtualOrder(self._next_oid(), ex, side, "IOC", price, qty, ts)

    def _find_order(self, oid: Optional[int]) -> Optional[VirtualOrder]:
        if oid is None:
            return None
        for o in self._orders:
            if o.oid == oid:
                return o
        return None

    def _get_book(self, exchange: str) -> BookSnapshot:
        return self._bin if exchange == "BINANCE" else self._kra

    # ── Fee helpers ──────────────────────────────────────────────────────────

    def _taker_fee(self, exchange: str) -> float:
        return (self.cfg.binance_taker_fee_bps if exchange == "BINANCE"
                else self.cfg.kraken_taker_fee_bps)

    def _maker_fee(self, exchange: str) -> float:
        return (self.cfg.binance_maker_fee_bps if exchange == "BINANCE"
                else self.cfg.kraken_maker_fee_bps)

    # ── Drawdown tracking ─────────────────────────────────────────────────────

    def _update_drawdown(self) -> None:
        self._peak_pnl    = max(self._peak_pnl, self._pnl)
        drawdown          = self._pnl - self._peak_pnl
        self._max_drawdown = min(self._max_drawdown, drawdown)

    # ── Output builders ──────────────────────────────────────────────────────

    def _build_trades_df(self) -> pl.DataFrame:
        if not self._trades:
            return pl.DataFrame()
        return pl.DataFrame({
            "open_ns":       [t.open_ns       for t in self._trades],
            "close_ns":      [t.close_ns      for t in self._trades],
            "buy_exchange":  [t.buy_exchange  for t in self._trades],
            "sell_exchange": [t.sell_exchange for t in self._trades],
            "buy_price":     [t.buy_price     for t in self._trades],
            "sell_price":    [t.sell_price    for t in self._trades],
            "qty":           [t.qty           for t in self._trades],
            "gross_pnl":     [t.gross_pnl     for t in self._trades],
            "fee":           [t.total_fee     for t in self._trades],
            "net_pnl":       [t.net_pnl       for t in self._trades],
            "hold_time_ms":  [t.hold_time_ms  for t in self._trades],
            "mode":          [t.mode          for t in self._trades],
        })

    def _build_equity_curve(self) -> pl.DataFrame:
        if not self._trades:
            return pl.DataFrame()
        ts   = [t.close_ns for t in self._trades]
        pnl  = np.cumsum([t.net_pnl for t in self._trades])
        return pl.DataFrame({"timestamp_ns": ts, "cumulative_pnl": pnl})

    def _compute_metrics(self) -> dict:
        if not self._trades:
            return {"error": "no trades"}

        net_pnls = np.array([t.net_pnl for t in self._trades])
        wins     = net_pnls[net_pnls > 0]
        losses   = net_pnls[net_pnls <= 0]

        hold_ms  = np.array([t.hold_time_ms for t in self._trades])

        # Annualised Sharpe (assumes trades are IID daily samples)
        mean_pnl  = np.mean(net_pnls)
        std_pnl   = np.std(net_pnls, ddof=1) if len(net_pnls) > 1 else 1e-9
        sharpe    = (mean_pnl / std_pnl) * math.sqrt(252 * 24)  # hourly-ish

        taker_trades = [t for t in self._trades if t.mode == "TAKER"]
        mm_trades    = [t for t in self._trades if t.mode == "MM"]

        return {
            "total_trades":       len(self._trades),
            "taker_trades":       len(taker_trades),
            "mm_trades":          len(mm_trades),
            "total_net_pnl":      float(np.sum(net_pnls)),
            "avg_net_pnl":        float(mean_pnl),
            "std_pnl":            float(std_pnl),
            "sharpe_annualized":  float(sharpe),
            "win_rate":           float(len(wins) / len(net_pnls)),
            "avg_win":            float(np.mean(wins))   if len(wins)   else 0.0,
            "avg_loss":           float(np.mean(losses)) if len(losses) else 0.0,
            "profit_factor":      float(np.sum(wins) / abs(np.sum(losses))) if losses.size else float("inf"),
            "max_drawdown_usd":   self._max_drawdown,
            "avg_hold_time_ms":   float(np.mean(hold_ms)),
            "median_hold_time_ms": float(np.median(hold_ms)),
            "gross_pnl":          float(np.sum([t.gross_pnl for t in self._trades])),
            "total_fees":         float(np.sum([t.total_fee for t in self._trades])),
        }


# ── Helpers ──────────────────────────────────────────────────────────────────


def _spread_bps(buy_px: float, sell_px: float) -> float:
    if buy_px <= 0:
        return 0.0
    return (sell_px - buy_px) / buy_px * 10_000.0


def load_ticks(path: str) -> pl.DataFrame:
    """Load tick data from Parquet or CSV, normalise column names."""
    p = Path(path)
    if p.suffix == ".parquet":
        df = pl.read_parquet(path)
    elif p.suffix in (".csv", ".tsv"):
        df = pl.read_csv(path)
    else:
        raise ValueError(f"Unsupported file format: {p.suffix}")
    return df


def merge_and_sort(*tick_dfs: pl.DataFrame) -> pl.DataFrame:
    """Merge multiple exchange tick DataFrames and sort by timestamp."""
    return pl.concat(list(tick_dfs)).sort("timestamp_ns")


def print_metrics(metrics: dict) -> None:
    print("\n" + "=" * 60)
    print("  BACKTEST RESULTS")
    print("=" * 60)
    for k, v in metrics.items():
        if isinstance(v, float):
            print(f"  {k:35s}: {v:>12.4f}")
        else:
            print(f"  {k:35s}: {v:>12}")
    print("=" * 60 + "\n")


# ── CLI ───────────────────────────────────────────────────────────────────────


def _parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Perp arb backtest")
    p.add_argument("--binance",      required=True, help="Binance perp tick file")
    p.add_argument("--kraken",       required=True, help="Kraken futures tick file")
    p.add_argument("--qty",          type=float, default=0.001)
    p.add_argument("--taker-bps",    type=float, default=12.0, dest="taker_bps")
    p.add_argument("--mm-bps",       type=float, default=6.0,  dest="mm_bps")
    p.add_argument("--latency-ms",   type=float, default=5.0,  dest="latency_ms")
    p.add_argument("--output-trades",  default=None, help="Save trade log to Parquet")
    p.add_argument("--output-equity",  default=None, help="Save equity curve to Parquet")
    return p.parse_args()


def main() -> None:
    args = _parse_args()

    cfg = BacktestConfig(
        trade_qty=args.qty,
        taker_threshold_bps=args.taker_bps,
        mm_spread_target_bps=args.mm_bps,
        latency_ns=int(args.latency_ms * 1_000_000),
    )

    print(f"Loading Binance ticks from {args.binance}...")
    bin_ticks = load_ticks(args.binance)
    if "exchange" not in bin_ticks.columns:
        bin_ticks = bin_ticks.with_columns(pl.lit("BINANCE").alias("exchange"))

    print(f"Loading Kraken ticks from {args.kraken}...")
    kra_ticks = load_ticks(args.kraken)
    if "exchange" not in kra_ticks.columns:
        kra_ticks = kra_ticks.with_columns(pl.lit("KRAKEN").alias("exchange"))

    ticks = merge_and_sort(bin_ticks, kra_ticks)
    print(f"Total ticks: {len(ticks):,}  ({ticks['timestamp_ns'].min()} → {ticks['timestamp_ns'].max()})")

    engine = PerpArbBacktest(cfg)
    results = engine.run(ticks)

    print_metrics(results["metrics"])

    if args.output_trades and not results["trades"].is_empty():
        results["trades"].write_parquet(args.output_trades)
        print(f"Trade log saved to {args.output_trades}")

    if args.output_equity and not results["equity_curve"].is_empty():
        results["equity_curve"].write_parquet(args.output_equity)
        print(f"Equity curve saved to {args.output_equity}")


if __name__ == "__main__":
    main()
