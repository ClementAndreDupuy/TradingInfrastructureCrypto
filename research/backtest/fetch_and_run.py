"""
Fetch live L2 snapshots from Binance BTCUSDT perp + Kraken PI_XBTUSD futures,
save to Parquet, then run the perp-arb backtest.

Usage:
    python3 research/backtest/fetch_and_run.py [--ticks N] [--interval-ms M]
"""
from __future__ import annotations

import argparse
import time
from pathlib import Path

import polars as pl
import requests

BINANCE_DEPTH_URL = "https://fapi.binance.com/fapi/v1/depth"
KRAKEN_TICKER_URL = "https://futures.kraken.com/derivatives/api/v3/orderbook"

DATA_DIR = Path(__file__).parent.parent.parent / "data"


def fetch_binance_snapshot() -> dict | None:
    try:
        r = requests.get(BINANCE_DEPTH_URL,
                         params={"symbol": "BTCUSDT", "limit": 5},
                         timeout=5)
        r.raise_for_status()
        d = r.json()
        best_bid = float(d["bids"][0][0])
        best_ask = float(d["asks"][0][0])
        bid_size = float(d["bids"][0][1])
        ask_size = float(d["asks"][0][1])
        return {
            "timestamp_ns": time.time_ns(),
            "exchange":     "BINANCE",
            "best_bid":     best_bid,
            "best_ask":     best_ask,
            "bid_size_1":   bid_size,
            "ask_size_1":   ask_size,
        }
    except Exception as e:
        print(f"  [WARN] Binance fetch failed: {e}")
        return None


def fetch_kraken_snapshot() -> dict | None:
    try:
        r = requests.get(KRAKEN_TICKER_URL,
                         params={"symbol": "PI_XBTUSD"},
                         timeout=5)
        r.raise_for_status()
        d = r.json()
        bids = sorted(d["orderBook"]["bids"], key=lambda x: -float(x[0]))
        asks = sorted(d["orderBook"]["asks"], key=lambda x:  float(x[0]))
        if not bids or not asks:
            return None
        best_bid = float(bids[0][0])
        best_ask = float(asks[0][0])
        bid_size = float(bids[0][1])
        ask_size = float(asks[0][1])
        return {
            "timestamp_ns": time.time_ns(),
            "exchange":     "KRAKEN",
            "best_bid":     best_bid,
            "best_ask":     best_ask,
            "bid_size_1":   bid_size,
            "ask_size_1":   ask_size,
        }
    except Exception as e:
        print(f"  [WARN] Kraken fetch failed: {e}")
        return None


def collect_ticks(n_ticks: int, interval_ms: int) -> pl.DataFrame:
    rows: list[dict] = []
    interval_s = interval_ms / 1000.0
    print(f"Collecting {n_ticks} snapshots per exchange "
          f"(interval {interval_ms} ms)…")

    for i in range(n_ticks):
        b = fetch_binance_snapshot()
        k = fetch_kraken_snapshot()
        if b:
            rows.append(b)
        if k:
            rows.append(k)
        if (i + 1) % 10 == 0:
            print(f"  {i + 1}/{n_ticks} snapshots collected")
        if i < n_ticks - 1:
            time.sleep(interval_s)

    df = pl.DataFrame(rows).sort("timestamp_ns")
    print(f"Collected {len(df)} ticks total.\n")
    return df


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ticks",       type=int,   default=60,
                    help="Snapshots per exchange (default 60)")
    ap.add_argument("--interval-ms", type=int,   default=1000,
                    help="Poll interval in ms (default 1000)")
    ap.add_argument("--qty",         type=float, default=0.001)
    ap.add_argument("--taker-bps",   type=float, default=12.0, dest="taker_bps")
    ap.add_argument("--mm-bps",      type=float, default=6.0,  dest="mm_bps")
    ap.add_argument("--maker-fee",   type=float, default=2.0,  dest="maker_fee",
                    help="Maker fee bps per leg (default 2.0)")
    ap.add_argument("--taker-fee",   type=float, default=5.0,  dest="taker_fee",
                    help="Taker fee bps per leg (default 5.0)")
    ap.add_argument("--save-data",   action="store_true",
                    help="Persist tick Parquet to data/")
    ap.add_argument("--engine",      choices=["cpp", "python"], default="cpp",
                    help="Simulation engine: cpp (C++ OrderBook+Risk) or python (default: cpp)")
    args = ap.parse_args()

    ticks = collect_ticks(args.ticks, args.interval_ms)

    if args.save_data:
        DATA_DIR.mkdir(parents=True, exist_ok=True)
        out = DATA_DIR / "perp_arb_ticks.parquet"
        ticks.write_parquet(out)
        print(f"Ticks saved → {out}\n")

    # ── run backtest ──────────────────────────────────────────────────────────
    import sys
    sys.path.insert(0, str(Path(__file__).parent.parent.parent))
    from research.backtest.perp_arb_backtest import (
        BacktestConfig, PerpArbBacktest, print_metrics,
    )

    cfg = BacktestConfig(
        trade_qty=args.qty,
        taker_threshold_bps=args.taker_bps,
        mm_spread_target_bps=args.mm_bps,
        binance_maker_fee_bps=args.maker_fee,
        binance_taker_fee_bps=args.taker_fee,
        kraken_maker_fee_bps=args.maker_fee,
        kraken_taker_fee_bps=args.taker_fee,
    )

    engine = PerpArbBacktest(cfg)
    if args.engine == "cpp":
        print("Engine: C++ (OrderBook + ArbRiskManager)")
        results = engine.run_cpp(ticks)
    else:
        print("Engine: Python")
        results = engine.run(ticks)
    print_metrics(results["metrics"])

    trades = results["trades"]
    if not trades.is_empty():
        print(trades.select([
            "open_ns", "buy_exchange", "sell_exchange",
            "buy_price", "sell_price", "net_pnl", "mode",
        ]))
    else:
        print("No trades executed — spread thresholds not crossed in this window.")


if __name__ == "__main__":
    main()