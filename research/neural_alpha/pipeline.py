"""
End-to-end neural alpha pipeline.

Steps:
    1. Fetch L5 LOB data from Binance/Kraken REST (extends fetch_and_run.py pattern)
    2. Build feature tensors (LOB + tick scalars)
    3. Train CryptoAlphaNet with walk-forward cross-validation
    4. Run event-driven backtest on out-of-sample predictions
    5. Compute alpha regression (IC, ICIR, OLS)
    6. Print full report

Usage:
    python -m research.neural_alpha.pipeline [options]
    python research/neural_alpha/pipeline.py --ticks 200 --epochs 10
"""
from __future__ import annotations

import argparse
import time
from pathlib import Path

import numpy as np
import polars as pl
import requests

from .core_bridge import CoreBridge

# ── Data fetcher ──────────────────────────────────────────────────────────────

BINANCE_DEPTH_URL = "https://fapi.binance.com/fapi/v1/depth"
BINANCE_SPOT_DEPTH_URL = "https://api.binance.com/api/v3/depth"
KRAKEN_DEPTH_URL = "https://futures.kraken.com/derivatives/api/v3/orderbook"
OKX_DEPTH_URL = "https://www.okx.com/api/v5/market/books"
COINBASE_DEPTH_URL = "https://api.exchange.coinbase.com/products/{product_id}/book"
N_LEVELS = 5


def _parse_binance_l5(d: dict, symbol: str, exchange_label: str) -> dict:
    bids = d["bids"][:N_LEVELS]
    asks = d["asks"][:N_LEVELS]
    row: dict = {
        "timestamp_ns": time.time_ns(),
        "exchange": exchange_label,
        "symbol": symbol,
        "best_bid": float(bids[0][0]),
        "best_ask": float(asks[0][0]),
    }
    for i, (bp, bs) in enumerate(bids, 1):
        row[f"bid_price_{i}"] = float(bp)
        row[f"bid_size_{i}"] = float(bs)
    for i, (ap, as_) in enumerate(asks, 1):
        row[f"ask_price_{i}"] = float(ap)
        row[f"ask_size_{i}"] = float(as_)
    return row




def _binance_symbol(symbol: str) -> str:
    return symbol.replace("-", "").upper()


def _okx_symbol(symbol: str) -> str:
    clean = symbol.replace("-", "").upper()
    if clean.endswith("USDT"):
        return f"{clean[:-4]}-USDT"
    if clean.endswith("USD"):
        return f"{clean[:-3]}-USD"
    return clean


def _coinbase_symbol(symbol: str) -> str:
    clean = symbol.replace("-", "").upper()
    if clean.endswith("USDT"):
        return f"{clean[:-4]}-USD"
    if clean.endswith("USD"):
        return f"{clean[:-3]}-USD"
    return clean


def _symbol_family(symbol: str) -> str:
    """Normalise venue-specific symbols (e.g. XBTUSD / BTC-USD / BTCUSDT) to basequote."""
    s = symbol.upper().replace("-", "").replace("_", "")
    if s.startswith("PI_"):
        s = s[3:]
    s = s.replace("XBT", "BTC")
    if s.endswith("PERP"):
        s = s[:-4]
    return s


def _kraken_symbol(symbol: str) -> str:
    clean = symbol.replace("-", "").upper()
    if clean.endswith("USDT"):
        base = clean[:-4]
    elif clean.endswith("USD"):
        base = clean[:-3]
    else:
        base = clean
    if base == "BTC":
        base = "XBT"
    return f"PI_{base}USD"

def _fetch_binance_l5(symbol: str = "BTCUSDT") -> dict | None:
    try:
        r = requests.get(
            BINANCE_DEPTH_URL,
            params={"symbol": _binance_symbol(symbol), "limit": N_LEVELS},
            timeout=5,
        )
        r.raise_for_status()
        return _parse_binance_l5(r.json(), _binance_symbol(symbol), "BINANCE")
    except Exception as e:
        print(f"  [WARN] Binance L5 fetch: {e}")
        return None


def _fetch_solana_l5(symbol: str = "SOLUSDT") -> dict | None:
    """Fetch L5 LOB snapshot for SOLUSDT from Binance spot."""
    try:
        r = requests.get(
            BINANCE_SPOT_DEPTH_URL,
            params={"symbol": _binance_symbol(symbol), "limit": N_LEVELS},
            timeout=5,
        )
        r.raise_for_status()
        return _parse_binance_l5(r.json(), _binance_symbol(symbol), "SOLANA")
    except Exception as e:
        print(f"  [WARN] Solana L5 fetch: {e}")
        return None


def _fetch_kraken_l5(symbol: str = "BTCUSDT") -> dict | None:
    try:
        r = requests.get(
            KRAKEN_DEPTH_URL,
            params={"symbol": _kraken_symbol(symbol)},
            timeout=5,
        )
        r.raise_for_status()
        d = r.json()
        bids = sorted(d["orderBook"]["bids"], key=lambda x: -float(x[0]))[:N_LEVELS]
        asks = sorted(d["orderBook"]["asks"], key=lambda x: float(x[0]))[:N_LEVELS]
        if not bids or not asks:
            return None
        row: dict = {
            "timestamp_ns": time.time_ns(),
            "exchange": "KRAKEN",
            "symbol": _kraken_symbol(symbol),
            "best_bid": float(bids[0][0]),
            "best_ask": float(asks[0][0]),
        }
        for i, (bp, bs) in enumerate(bids, 1):
            row[f"bid_price_{i}"] = float(bp)
            row[f"bid_size_{i}"] = float(bs)
        for i, (ap, as_) in enumerate(asks, 1):
            row[f"ask_price_{i}"] = float(ap)
            row[f"ask_size_{i}"] = float(as_)
        return row
    except Exception as e:
        print(f"  [WARN] Kraken L5 fetch: {e}")
        return None


def _fetch_okx_l5(symbol: str = "BTCUSDT") -> dict | None:
    try:
        r = requests.get(OKX_DEPTH_URL, params={"instId": _okx_symbol(symbol), "sz": N_LEVELS}, timeout=5)
        r.raise_for_status()
        data = r.json().get("data", [])
        if not data:
            return None
        book = data[0]
        bids = [(float(px), float(sz)) for px, sz, *_ in book.get("bids", [])][:N_LEVELS]
        asks = [(float(px), float(sz)) for px, sz, *_ in book.get("asks", [])][:N_LEVELS]
        if not bids or not asks:
            return None
        row: dict = {
            "timestamp_ns": time.time_ns(),
            "exchange": "OKX",
            "symbol": _okx_symbol(symbol),
            "best_bid": bids[0][0],
            "best_ask": asks[0][0],
        }
        for i, (bp, bs) in enumerate(bids, 1):
            row[f"bid_price_{i}"] = bp
            row[f"bid_size_{i}"] = bs
        for i, (ap, as_) in enumerate(asks, 1):
            row[f"ask_price_{i}"] = ap
            row[f"ask_size_{i}"] = as_
        return row
    except Exception as e:
        print(f"  [WARN] OKX L5 fetch: {e}")
        return None


def _fetch_coinbase_l5(symbol: str = "BTCUSDT") -> dict | None:
    try:
        product_id = _coinbase_symbol(symbol)
        r = requests.get(COINBASE_DEPTH_URL.format(product_id=product_id), params={"level": 2}, timeout=5)
        r.raise_for_status()
        d = r.json()
        bids = [(float(px), float(sz)) for px, sz, *_ in d.get("bids", [])][:N_LEVELS]
        asks = [(float(px), float(sz)) for px, sz, *_ in d.get("asks", [])][:N_LEVELS]
        if not bids or not asks:
            return None
        row: dict = {
            "timestamp_ns": time.time_ns(),
            "exchange": "COINBASE",
            "symbol": product_id,
            "best_bid": bids[0][0],
            "best_ask": asks[0][0],
        }
        for i, (bp, bs) in enumerate(bids, 1):
            row[f"bid_price_{i}"] = bp
            row[f"bid_size_{i}"] = bs
        for i, (ap, as_) in enumerate(asks, 1):
            row[f"ask_price_{i}"] = ap
            row[f"ask_size_{i}"] = as_
        return row
    except Exception as e:
        print(f"  [WARN] Coinbase L5 fetch: {e}")
        return None


def collect_from_core_bridge(n_ticks: int, interval_ms: int) -> pl.DataFrame | None:
    bridge = CoreBridge()
    if not bridge.open():
        return None

    rows: list[dict] = []
    interval_s = interval_ms / 1000.0
    while len(rows) < n_ticks:
        rows.extend(bridge.read_new_ticks())
        if len(rows) >= n_ticks:
            break
        time.sleep(interval_s)
    bridge.close()

    if not rows:
        return None
    return pl.DataFrame(rows[:n_ticks]).sort("timestamp_ns")


def _enforce_symbol_and_exchange_coverage(
    df: pl.DataFrame,
    exchanges: list[str],
    symbol: str,
) -> pl.DataFrame:
    wanted_symbol_family = _symbol_family(symbol)
    wanted_exchanges = {ex.upper() for ex in exchanges}

    filtered = [
        row
        for row in df.to_dicts()
        if row.get("exchange", "").upper() in wanted_exchanges
        and _symbol_family(str(row.get("symbol", ""))) == wanted_symbol_family
    ]
    out = pl.DataFrame(filtered).sort("timestamp_ns") if filtered else pl.DataFrame([])

    if len(out) == 0:
        return out

    found_exchanges = {ex.upper() for ex in out["exchange"].to_list()}
    missing = sorted(wanted_exchanges - found_exchanges)
    if missing:
        raise RuntimeError(
            "Dataset does not include all configured exchanges for requested symbol "
            f"{symbol}: missing={missing}, found={sorted(found_exchanges)}"
        )
    return out


def _collect_l5_ticks_rest(n_ticks: int, interval_ms: int, exchanges: list[str], symbol: str = "BTCUSDT") -> pl.DataFrame:
    rows: list[dict] = []
    interval_s = interval_ms / 1000.0
    _fetchers = {
        "BINANCE": _fetch_binance_l5,
        "KRAKEN": _fetch_kraken_l5,
        "OKX": _fetch_okx_l5,
        "COINBASE": _fetch_coinbase_l5,
        "SOLANA": _fetch_solana_l5,
    }

    print(f"Collecting {n_ticks} L5 snapshots per exchange over REST (interval {interval_ms} ms)…")
    for i in range(n_ticks):
        for ex in exchanges:
            fetcher = _fetchers.get(ex)
            if fetcher is None:
                print(f"  [WARN] Unknown exchange: {ex}")
                continue
            row = fetcher(symbol)
            if row:
                rows.append(row)
        if i < n_ticks - 1:
            time.sleep(interval_s)

    if not rows:
        raise RuntimeError("No data collected — check network and exchange APIs.")
    return pl.DataFrame(rows).sort("timestamp_ns")


def collect_l5_ticks(
    n_ticks: int,
    interval_ms: int,
    exchanges: list[str] | None = None,
    allow_rest_fallback: bool = True,
    symbol: str = "BTCUSDT",
) -> pl.DataFrame:
    """Fetch L5 LOB snapshots from core feed bridge, optionally topped up via REST."""
    exchanges = exchanges or ["BINANCE", "KRAKEN", "OKX", "COINBASE"]
    bridge_df = collect_from_core_bridge(n_ticks=n_ticks, interval_ms=interval_ms)

    if bridge_df is not None and len(bridge_df) >= n_ticks // 2:
        bridge_df = _enforce_symbol_and_exchange_coverage(bridge_df, exchanges, symbol)
        if len(bridge_df) < n_ticks and allow_rest_fallback:
            print(f"[WARN] bridge returned {len(bridge_df)} ticks (< {n_ticks}); topping up with REST")
            rest_df = _collect_l5_ticks_rest(n_ticks - len(bridge_df), interval_ms, exchanges, symbol=symbol)
            merged_df = pl.concat([bridge_df, rest_df]).sort("timestamp_ns")
            return _enforce_symbol_and_exchange_coverage(merged_df, exchanges, symbol)
        if len(bridge_df) < n_ticks:
            raise RuntimeError(
                f"Core bridge returned {len(bridge_df)} ticks (< {n_ticks}) and REST fallback is disabled."
            )
        return bridge_df

    if allow_rest_fallback:
        print("[WARN] core bridge unavailable or insufficient; falling back to REST collection")
        rest_df = _collect_l5_ticks_rest(n_ticks, interval_ms, exchanges, symbol=symbol)
        return _enforce_symbol_and_exchange_coverage(rest_df, exchanges, symbol)

    raise RuntimeError("Core bridge unavailable and REST fallback disabled.")


# ── Synthetic data for offline testing ───────────────────────────────────────

def generate_synthetic_lob(n_ticks: int = 1000, seed: int = 42) -> pl.DataFrame:
    """
    Generate a synthetic L5 LOB dataset for offline testing.
    Prices follow a mean-reverting process; sizes are log-normal.
    """
    rng = np.random.default_rng(seed)
    mid = 50000.0
    rows = []
    for t in range(n_ticks):
        mid += rng.normal(0, 5)   # random walk
        spread = abs(rng.normal(10, 3)) + 1.0
        row: dict = {
            "timestamp_ns": int(1_700_000_000_000_000_000 + t * 500_000_000),
            "exchange":     "BINANCE",
            "best_bid":     mid - spread / 2,
            "best_ask":     mid + spread / 2,
        }
        for i in range(1, N_LEVELS + 1):
            row[f"bid_price_{i}"] = mid - spread / 2 - (i - 1) * spread
            row[f"bid_size_{i}"]  = float(rng.lognormal(1, 0.5))
            row[f"ask_price_{i}"] = mid + spread / 2 + (i - 1) * spread
            row[f"ask_size_{i}"]  = float(rng.lognormal(1, 0.5))
        rows.append(row)

    return pl.DataFrame(rows)


# ── Walk-forward test slice tracking ─────────────────────────────────────────

def _fold_slices(T: int, n_folds: int, train_frac: float) -> list[tuple[int, int]]:
    """Return (start, end) index of each test slice."""
    fold_size = T // n_folds
    slices = []
    for i in range(n_folds):
        end_test  = (i + 1) * fold_size
        start_test = int(end_test - fold_size * (1 - train_frac))
        slices.append((start_test, end_test))
    return slices


# ── Main pipeline ─────────────────────────────────────────────────────────────

def run_pipeline(args: argparse.Namespace) -> None:
    from .alpha_regression import analyse_alpha, print_alpha_report
    from .backtest import BacktestConfig, NeuralAlphaBacktest
    from .trainer import TrainerConfig, walk_forward_train

    # ── 1. Data ──────────────────────────────────────────────────────────────
    if args.synthetic:
        print("Using synthetic LOB data.")
        df = generate_synthetic_lob(n_ticks=args.ticks)
    elif args.data_path and Path(args.data_path).exists():
        print(f"Loading data from {args.data_path}")
        df = pl.read_parquet(args.data_path)
    else:
        df = collect_l5_ticks(
            args.ticks,
            args.interval_ms,
            exchanges=args.exchanges.split(","),
            symbol=args.symbol,
            allow_rest_fallback=not args.core_only,
        )

    if args.save_data:
        out = Path(args.save_data)
        out.parent.mkdir(parents=True, exist_ok=True)
        df.write_parquet(str(out))
        print(f"Data saved → {out}")

    print(f"Dataset: {len(df)} ticks  columns={df.columns[:8]}…\n")

    # ── 2. Train (walk-forward) ───────────────────────────────────────────────
    trainer_cfg = TrainerConfig(
        epochs=args.epochs,
        n_folds=args.folds,
        pretrain=not args.no_pretrain,
        pretrain_epochs=args.pretrain_epochs,
        d_spatial=args.d_spatial,
        d_temporal=args.d_temporal,
        seq_len=args.seq_len,
        batch_size=args.batch_size,
        lr=args.lr,
    )

    fold_results = walk_forward_train(df, trainer_cfg)

    if not fold_results:
        print("No fold results — dataset too small for the chosen seq_len / n_folds.")
        return

    # ── 3. Backtest ───────────────────────────────────────────────────────────
    bt_cfg = BacktestConfig(
        entry_threshold_bps=args.entry_bps,
        taker_fee_bps=args.fee_bps,
    )
    test_slices = _fold_slices(len(df), args.folds, trainer_cfg.train_frac)

    all_bt_metrics: list[dict] = []
    for fold, (start, end) in zip(fold_results, test_slices):
        test_df  = df[start:end]
        signals  = fold["predictions"][:, 2]  # mid-horizon (index 2 = 100t)

        # Expand per-window predictions to per-tick (last-tick alignment)
        T_test = len(test_df)
        tick_signals = np.zeros(T_test, dtype=np.float32)
        for i, sig in enumerate(signals):
            idx = min(i + trainer_cfg.seq_len - 1, T_test - 1)
            tick_signals[idx] = sig

        bt = NeuralAlphaBacktest(bt_cfg)
        bt_result = bt.run(test_df, tick_signals)
        all_bt_metrics.append(bt_result["metrics"])
        print(f"Fold {fold['fold']} backtest: {bt_result['metrics']}")

    # Aggregate backtest metrics across folds
    merged_bt: dict = {}
    float_keys = [k for k in all_bt_metrics[0] if isinstance(all_bt_metrics[0][k], float)]
    for k in float_keys:
        vals = [m[k] for m in all_bt_metrics if k in m]
        merged_bt[k] = float(np.mean(vals)) if vals else 0.0
    merged_bt["total_trades"] = sum(m.get("total_trades", 0) for m in all_bt_metrics)

    # ── 4. Alpha regression ───────────────────────────────────────────────────
    alpha_metrics = analyse_alpha(fold_results, horizon_idx=1)

    # ── 5. Report ─────────────────────────────────────────────────────────────
    print_alpha_report(alpha_metrics, merged_bt)

    # ── 6. Save model (best fold by validation loss) ──────────────────────────
    if args.save_model:
        import torch
        best_fold = min(fold_results, key=lambda f: f["metrics"].get("loss_total", 1e9))
        torch.save(best_fold["model_state"], args.save_model)
        print(f"Best model saved → {args.save_model}")


def main() -> None:
    ap = argparse.ArgumentParser(description="Neural crypto alpha pipeline")
    ap.add_argument("--ticks",           type=int,   default=300,
                    help="Ticks to collect per exchange (default 300)")
    ap.add_argument("--interval-ms",     type=int,   default=500,   dest="interval_ms")
    ap.add_argument("--symbol",          type=str,   default="BTCUSDT")
    ap.add_argument("--exchanges", type=str, default="BINANCE,KRAKEN,OKX,COINBASE")
    ap.add_argument("--synthetic",       action="store_true",
                    help="Use synthetic LOB data (offline testing)")
    ap.add_argument("--core-only", action="store_true",
                    help="Require core feed bridge data; disable REST fallback")
    ap.add_argument("--data-path",       type=str,   default=None,  dest="data_path",
                    help="Load existing Parquet instead of fetching")
    ap.add_argument("--save-data",       type=str,   default=None,  dest="save_data")
    ap.add_argument("--save-model",      type=str,   default=None,  dest="save_model")

    # Model
    ap.add_argument("--d-spatial",       type=int,   default=64,    dest="d_spatial")
    ap.add_argument("--d-temporal",      type=int,   default=128,   dest="d_temporal")
    ap.add_argument("--seq-len",         type=int,   default=64,    dest="seq_len")

    # Training
    ap.add_argument("--epochs",          type=int,   default=20)
    ap.add_argument("--folds",           type=int,   default=4)
    ap.add_argument("--batch-size",      type=int,   default=32,    dest="batch_size")
    ap.add_argument("--lr",              type=float, default=3e-4)
    ap.add_argument("--no-pretrain",     action="store_true",       dest="no_pretrain")
    ap.add_argument("--pretrain-epochs", type=int,   default=5,     dest="pretrain_epochs")

    # Backtest
    ap.add_argument("--entry-bps",       type=float, default=5.0,   dest="entry_bps")
    ap.add_argument("--fee-bps",         type=float, default=5.0,   dest="fee_bps")

    args = ap.parse_args()
    run_pipeline(args)


if __name__ == "__main__":
    main()
