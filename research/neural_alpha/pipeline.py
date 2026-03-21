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
from typing import TYPE_CHECKING
import numpy as np
import polars as pl
import requests

try:
    import trading_core as _tc
except ImportError:
    _tc = None
from .runtime.core_bridge import CoreBridge

if TYPE_CHECKING:
    from .models.trainer import TrainerConfig
BINANCE_DEPTH_URL = "https://fapi.binance.com/fapi/v1/depth"
KRAKEN_DEPTH_URL = "https://api.kraken.com/0/public/Depth"
OKX_DEPTH_URL = "https://www.okx.com/api/v5/market/books"
COINBASE_DEPTH_URL = "https://api.coinbase.com/api/v3/brokerage/market/product_book"
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
    if _tc is not None:
        return _tc.SymbolMapper.map_for_exchange(_tc.Exchange.BINANCE, symbol)
    return symbol.replace("-", "").upper()


def _okx_symbol(symbol: str) -> str:
    if _tc is not None:
        return _tc.SymbolMapper.map_for_exchange(_tc.Exchange.OKX, symbol)
    clean = symbol.replace("-", "").upper()
    if clean.endswith("USDT"):
        return f"{clean[:-4]}-USDT"
    if clean.endswith("USD"):
        return f"{clean[:-3]}-USD"
    return clean


def _coinbase_symbol(symbol: str) -> str:
    if _tc is not None:
        return _tc.SymbolMapper.map_for_exchange(_tc.Exchange.COINBASE, symbol)
    clean = symbol.replace("-", "").upper()
    if clean.endswith("USDT"):
        return f"{clean[:-4]}-USDT"
    if clean.endswith("USD"):
        return f"{clean[:-3]}-USD"
    return clean


def _symbol_family(symbol: str) -> str:
    """Normalise venue-specific symbols (e.g. XBTUSD / BTC-USD / BTCUSDT) to basequote."""
    s = symbol.upper()
    if s.startswith("PI_"):
        s = s[3:]
    if s.endswith("PERP"):
        s = s[:-4]
    s = s.replace("XBT", "BTC")
    if _tc is not None:
        return _tc.SymbolMapper.map_all(s).binance
    return s.replace("-", "").replace("_", "").replace("/", "")


def _kraken_symbol(symbol: str) -> str:
    if _tc is not None:
        vs = _tc.SymbolMapper.map_all(symbol)
        (base, _, quote) = vs.kraken_rest.partition("/")
        if base == "BTC":
            base = "XBT"
        quote = quote.replace("USDT", "USD")
        return f"{base}{quote}"
    clean = symbol.replace("-", "").upper()
    if clean.endswith("USDT"):
        base = clean[:-4]
    elif clean.endswith("USD"):
        base = clean[:-3]
    else:
        base = clean
    if base == "BTC":
        base = "XBT"
    return f"{base}USD"


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


def _fetch_kraken_l5(symbol: str = "BTCUSDT") -> dict | None:
    try:
        pair = _kraken_symbol(symbol)
        r = requests.get(KRAKEN_DEPTH_URL, params={"pair": pair, "count": N_LEVELS}, timeout=5)
        r.raise_for_status()
        d = r.json()
        book = next(iter(d["result"].values()))
        bids = [[float(e[0]), float(e[1])] for e in book["bids"][:N_LEVELS]]
        asks = [[float(e[0]), float(e[1])] for e in book["asks"][:N_LEVELS]]
        if not bids or not asks:
            return None
        row: dict = {
            "timestamp_ns": time.time_ns(),
            "exchange": "KRAKEN",
            "symbol": pair,
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
        print(f"  [WARN] Kraken L5 fetch: {e}")
        return None


def _fetch_okx_l5(symbol: str = "BTCUSDT") -> dict | None:
    try:
        r = requests.get(
            OKX_DEPTH_URL, params={"instId": _okx_symbol(symbol), "sz": N_LEVELS}, timeout=5
        )
        r.raise_for_status()
        data = r.json().get("data", [])
        if not data:
            return None
        book = data[0]
        bids = [(float(px), float(sz)) for (px, sz, *_) in book.get("bids", [])][:N_LEVELS]
        asks = [(float(px), float(sz)) for (px, sz, *_) in book.get("asks", [])][:N_LEVELS]
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
        r = requests.get(
            COINBASE_DEPTH_URL, params={"product_id": product_id, "limit": N_LEVELS}, timeout=5
        )
        r.raise_for_status()
        pricebook = r.json().get("pricebook", {})
        bids = [(float(b["price"]), float(b["size"])) for b in pricebook.get("bids", [])][:N_LEVELS]
        asks = [(float(a["price"]), float(a["size"])) for a in pricebook.get("asks", [])][:N_LEVELS]
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
    df: pl.DataFrame, exchanges: list[str], symbol: str
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
            f"Dataset does not include all configured exchanges for requested symbol {symbol}: missing={missing}, found={sorted(found_exchanges)}"
        )
    return out


def _collect_l5_ticks_rest(
    n_ticks: int, interval_ms: int, exchanges: list[str], symbol: str = "BTCUSDT"
) -> pl.DataFrame:
    rows: list[dict] = []
    interval_s = interval_ms / 1000.0
    _fetchers = {
        "BINANCE": _fetch_binance_l5,
        "KRAKEN": _fetch_kraken_l5,
        "OKX": _fetch_okx_l5,
        "COINBASE": _fetch_coinbase_l5,
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
            print(
                f"[WARN] bridge returned {len(bridge_df)} ticks (< {n_ticks}); topping up with REST"
            )
            rest_df = _collect_l5_ticks_rest(
                n_ticks - len(bridge_df), interval_ms, exchanges, symbol=symbol
            )
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


def _fold_slices(T: int, n_folds: int, train_frac: float) -> list[tuple[int, int]]:
    """Return (start, end) index of each test slice."""
    fold_size = T // n_folds
    slices = []
    for i in range(n_folds):
        end_test = (i + 1) * fold_size
        start_test = int(end_test - fold_size * (1 - train_frac))
        slices.append((start_test, end_test))
    return slices


def _evaluate_state_on_holdout(df: pl.DataFrame, state_dict: dict, cfg: TrainerConfig) -> float:
    import torch
    from torch.utils.data import DataLoader
    from .data.dataset import DatasetConfig, LOBDataset
    from .models.model import CryptoAlphaNet
    from .models.trainer import selection_score

    dataset = LOBDataset(df, DatasetConfig(seq_len=cfg.seq_len))
    if len(dataset) == 0:
        return float("inf")
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    loader = DataLoader(dataset, batch_size=64, shuffle=False)
    model = (
        CryptoAlphaNet(
            d_spatial=cfg.d_spatial,
            d_temporal=cfg.d_temporal,
            n_lob_heads=cfg.n_lob_heads,
            n_lob_layers=cfg.n_lob_layers,
            n_temp_heads=cfg.n_temp_heads,
            n_temp_layers=cfg.n_temp_layers,
            dropout=cfg.dropout,
            seq_len=cfg.seq_len,
        )
        .to(device)
        .eval()
    )
    model.load_state_dict(state_dict, strict=False)
    totals: dict[str, float] = {
        "loss_return": 0.0,
        "loss_direction": 0.0,
        "loss_risk": 0.0,
        "loss_tc": 0.0,
    }
    n_batches = 0
    with torch.no_grad():
        for batch in loader:
            lob = batch["lob"].to(device)
            scalar = batch["scalar"].to(device)
            labels = batch["labels"].to(device)
            preds = model(lob, scalar)
            ret_diff = preds["returns"][:, -1, :] - labels[:, -1, :4]
            totals["loss_return"] += float((ret_diff * ret_diff).mean().item())
            dir_logits = preds["direction"][:, -1, :]
            dir_targets = labels[:, -1, 4].long()
            totals["loss_direction"] += float(
                torch.nn.functional.cross_entropy(dir_logits, dir_targets).item()
            )
            risk_pred = preds["risk"][:, -1]
            risk_targets = labels[:, -1, 5]
            totals["loss_risk"] += float(
                torch.nn.functional.binary_cross_entropy(risk_pred, risk_targets).item()
            )
            pred_ret_mid = preds["returns"][:, -1, 2]
            tc_threshold = cfg.tc_bps * 0.0001
            totals["loss_tc"] += float(
                torch.nn.functional.relu(tc_threshold - pred_ret_mid.abs()).mean().item()
            )
            n_batches += 1
    avg_metrics = {k: v / max(n_batches, 1) for (k, v) in totals.items()}
    return selection_score(avg_metrics, cfg)


def _atomic_torch_save(state_dict: dict, output_path: Path) -> None:
    import torch

    output_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = output_path.with_suffix(output_path.suffix + ".tmp")
    torch.save(state_dict, tmp_path)
    tmp_path.replace(output_path)


def _blend_fold_results(primary_folds: list[dict], secondary_folds: list[dict]) -> list[dict]:
    """Average prediction tensors from two fold-result lists."""
    if len(primary_folds) != len(secondary_folds):
        raise ValueError("Primary/secondary fold counts do not match for ensembling.")
    blended: list[dict] = []
    for primary, secondary in zip(primary_folds, secondary_folds):
        p_pred = primary.get("predictions")
        s_pred = secondary.get("predictions")
        p_dir = primary.get("direction_probs")
        s_dir = secondary.get("direction_probs")
        p_risk = primary.get("risk_scores")
        s_risk = secondary.get("risk_scores")
        p_lbl = primary.get("labels")
        s_lbl = secondary.get("labels")
        if (
            p_pred is None
            or s_pred is None
            or p_dir is None
            or s_dir is None
            or p_risk is None
            or s_risk is None
            or p_lbl is None
            or (s_lbl is None)
        ):
            raise ValueError("Missing predictions/labels while building fold ensemble.")
        if (
            p_pred.shape != s_pred.shape
            or p_dir.shape != s_dir.shape
            or p_risk.shape != s_risk.shape
            or p_lbl.shape != s_lbl.shape
        ):
            raise ValueError("Primary/secondary fold tensor shapes do not match.")
        blended.append(
            {
                **primary,
                "predictions": ((p_pred + s_pred) / 2.0).astype(np.float32),
                "direction_probs": ((p_dir + s_dir) / 2.0).astype(np.float32),
                "risk_scores": ((p_risk + s_risk) / 2.0).astype(np.float32),
                "labels": p_lbl,
                "ensemble": True,
            }
        )
    return blended


def _signal_from_fold_outputs(
    fold: dict,
    *,
    horizon_idx: int = 2,
    direction_confidence_floor: float = 0.50,
    risk_ceiling: float = 0.55,
) -> np.ndarray:
    returns = np.asarray(fold["predictions"][:, horizon_idx], dtype=np.float32)
    direction_probs = fold.get("direction_probs")
    risk_scores = fold.get("risk_scores")
    signal = returns.copy()

    if direction_probs is not None:
        direction_probs = np.asarray(direction_probs, dtype=np.float32)
        if direction_probs.ndim == 2 and direction_probs.shape[1] >= 3:
            long_conf = direction_probs[:, 2]
            short_conf = direction_probs[:, 0]
            sign_mismatch = (signal > 0.0) & (long_conf < direction_confidence_floor)
            sign_mismatch |= (signal < 0.0) & (short_conf < direction_confidence_floor)
            signal[sign_mismatch] = 0.0

    if risk_scores is not None:
        risk_scores = np.asarray(risk_scores, dtype=np.float32)
        safe_scale = np.clip(1.0 - risk_scores, 0.0, 1.0)
        signal *= safe_scale
        signal[risk_scores > risk_ceiling] = 0.0

    return signal.astype(np.float32)


def run_pipeline(args: argparse.Namespace) -> None:
    from research.regime import RegimeConfig, save_regime_artifact, train_regime_model_from_ipc
    from .evaluation.alpha_regression import analyse_alpha, print_alpha_report
    from .evaluation.backtest import BacktestConfig, NeuralAlphaBacktest
    from .models.trainer import TrainerConfig, walk_forward_train

    if args.data_path and Path(args.data_path).exists():
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
        w_return=args.primary_w_return,
        w_direction=args.primary_w_direction,
        w_risk=args.primary_w_risk,
        w_tc=args.primary_w_tc,
        selection_w_return=args.primary_selection_w_return,
        selection_w_direction=args.primary_selection_w_direction,
        selection_w_risk=args.primary_selection_w_risk,
        selection_w_tc=args.primary_selection_w_tc,
    )
    fold_results = walk_forward_train(df, trainer_cfg)
    if not fold_results:
        print("No fold results — dataset too small for the chosen seq_len / n_folds.")
        return
    secondary_fold_results: list[dict] | None = None
    effective_folds = fold_results
    if args.enable_secondary_model:
        print("Training secondary ensemble model (d_spatial=32, d_temporal=64, n_temp_layers=1)…")
        secondary_cfg = TrainerConfig(
            epochs=args.epochs,
            n_folds=args.folds,
            pretrain=not args.no_pretrain,
            pretrain_epochs=args.pretrain_epochs,
            d_spatial=32,
            d_temporal=64,
            n_temp_layers=1,
            seq_len=args.seq_len,
            batch_size=args.batch_size,
            lr=args.lr,
            w_return=args.secondary_w_return,
            w_direction=args.secondary_w_direction,
            w_risk=args.secondary_w_risk,
            w_tc=args.secondary_w_tc,
            selection_w_return=args.secondary_selection_w_return,
            selection_w_direction=args.secondary_selection_w_direction,
            selection_w_risk=args.secondary_selection_w_risk,
            selection_w_tc=args.secondary_selection_w_tc,
        )
        secondary_fold_results = walk_forward_train(df, secondary_cfg)
        if not secondary_fold_results:
            raise RuntimeError("Secondary ensemble model produced no fold results.")
        effective_folds = _blend_fold_results(fold_results, secondary_fold_results)
        print("Secondary model blended with primary predictions (50/50).")
    bt_cfg = BacktestConfig(entry_threshold_bps=args.entry_bps, taker_fee_bps=args.fee_bps)
    test_slices = _fold_slices(len(df), args.folds, trainer_cfg.train_frac)
    all_bt_metrics: list[dict] = []
    for fold, (start, end) in zip(effective_folds, test_slices):
        test_df = df[start:end]
        signals = _signal_from_fold_outputs(
            fold,
            horizon_idx=args.signal_horizon_idx,
            direction_confidence_floor=args.direction_confidence_floor,
            risk_ceiling=args.risk_ceiling,
        )
        T_test = len(test_df)
        tick_signals = np.zeros(T_test, dtype=np.float32)
        for i, sig in enumerate(signals):
            idx = min(i + trainer_cfg.seq_len - 1, T_test - 1)
            tick_signals[idx] = sig
        bt = NeuralAlphaBacktest(bt_cfg)
        bt_result = bt.run(test_df, tick_signals)
        all_bt_metrics.append(bt_result["metrics"])
        print(f"Fold {fold['fold']} backtest: {bt_result['metrics']}")
    merged_bt: dict = {}
    float_keys = [k for k in all_bt_metrics[0] if isinstance(all_bt_metrics[0][k], float)]
    for k in float_keys:
        vals = [m[k] for m in all_bt_metrics if k in m]
        merged_bt[k] = float(np.mean(vals)) if vals else 0.0
    merged_bt["total_trades"] = sum((m.get("total_trades", 0) for m in all_bt_metrics))
    alpha_metrics = analyse_alpha(effective_folds, horizon_idx=2)
    print_alpha_report(alpha_metrics, merged_bt)
    try:
        regime_cfg = RegimeConfig(n_regimes=args.regimes)
        (regime_artifact, regime_dist) = train_regime_model_from_ipc(args.ipc_dir, regime_cfg)
        save_regime_artifact(regime_artifact, args.save_regime_model)
        print(
            f"R2 regime model trained (n_regimes={regime_cfg.n_regimes}) from {args.ipc_dir} and saved → {args.save_regime_model}"
        )
        print(f"R2 regime distribution: {regime_dist}")
    except Exception as exc:
        print(f"[WARN] R2 regime training skipped: {exc}")
    if args.save_model:
        import torch

        best_fold = min(
            fold_results, key=lambda f: f.get("best_selection_score", 1000000000.0)
        )
        challenger_state = best_fold["model_state"]
        selected_state = challenger_state
        holdout_start = int(len(df) * 0.8)
        holdout_df = df[holdout_start:]
        incumbent_oos_mse: float | None = None
        challenger_oos_mse: float | None = None
        if len(holdout_df) >= trainer_cfg.seq_len * 2:
            challenger_oos_mse = _evaluate_state_on_holdout(
                holdout_df, challenger_state, trainer_cfg
            )
            incumbent_path = Path(args.save_model)
            if incumbent_path.exists():
                incumbent_state = torch.load(incumbent_path, map_location="cpu", weights_only=True)
                incumbent_oos_mse = _evaluate_state_on_holdout(
                    holdout_df, incumbent_state, trainer_cfg
                )
                if incumbent_oos_mse < challenger_oos_mse:
                    selected_state = incumbent_state
                    print(
                        f"[MODEL_SELECT] challenger rejected on holdout (incumbent_mse={incumbent_oos_mse:.6e}, challenger_mse={challenger_oos_mse:.6e})"
                    )
        primary_path = Path(args.save_model)
        _atomic_torch_save(selected_state, primary_path)
        print(f"Best primary model saved → {primary_path}")
        if challenger_oos_mse is not None:
            print(
                f"[MODEL_SELECT] holdout mid-return MSE incumbent={incumbent_oos_mse} challenger={challenger_oos_mse} selected={('incumbent' if selected_state is not challenger_state else 'challenger')}"
            )
        if args.enable_secondary_model and secondary_fold_results:
            secondary_cfg = TrainerConfig(
                epochs=args.epochs,
                n_folds=args.folds,
                pretrain=not args.no_pretrain,
                pretrain_epochs=args.pretrain_epochs,
                d_spatial=32,
                d_temporal=64,
                n_temp_layers=1,
                seq_len=args.seq_len,
                batch_size=args.batch_size,
                lr=args.lr,
                w_return=args.secondary_w_return,
                w_direction=args.secondary_w_direction,
                w_risk=args.secondary_w_risk,
                w_tc=args.secondary_w_tc,
                selection_w_return=args.secondary_selection_w_return,
                selection_w_direction=args.secondary_selection_w_direction,
                selection_w_risk=args.secondary_selection_w_risk,
                selection_w_tc=args.secondary_selection_w_tc,
            )
            best_secondary = min(
                secondary_fold_results, key=lambda f: f.get("best_selection_score", 1000000000.0)
            )
            secondary_path = primary_path.with_name(
                f"{primary_path.stem}_secondary{primary_path.suffix}"
            )
            secondary_state = best_secondary["model_state"]
            _atomic_torch_save(secondary_state, secondary_path)
            secondary_mse = None
            holdout_start = int(len(df) * 0.8)
            holdout_df = df[holdout_start:]
            if len(holdout_df) >= secondary_cfg.seq_len * 2:
                secondary_mse = _evaluate_state_on_holdout(
                    holdout_df, secondary_state, secondary_cfg
                )
            print(
                f"Secondary ensemble model saved → {secondary_path}"
                + (f" (holdout_mse={secondary_mse:.6e})" if secondary_mse is not None else "")
            )


def main() -> None:
    ap = argparse.ArgumentParser(description="Neural crypto alpha pipeline")
    ap.add_argument(
        "--ticks", type=int, default=300, help="Ticks to collect per exchange (default 300)"
    )
    ap.add_argument("--interval-ms", type=int, default=500, dest="interval_ms")
    ap.add_argument("--symbol", type=str, default="BTCUSDT")
    ap.add_argument("--exchanges", type=str, default="BINANCE,KRAKEN,OKX,COINBASE")
    ap.add_argument(
        "--core-only",
        action="store_true",
        help="Require core feed bridge data; disable REST fallback",
    )
    ap.add_argument(
        "--data-path",
        type=str,
        default=None,
        dest="data_path",
        help="Load existing Parquet instead of fetching",
    )
    ap.add_argument("--save-data", type=str, default=None, dest="save_data")
    ap.add_argument("--save-model", type=str, default=None, dest="save_model")
    ap.add_argument("--ipc-dir", type=str, default="/ipc", dest="ipc_dir")
    ap.add_argument("--regimes", type=int, default=4, help="R2 regime count (supported: 3 or 4)")
    ap.add_argument(
        "--save-regime-model",
        type=str,
        default="models/r2_regime_model.json",
        dest="save_regime_model",
        help="Path to save trained R2 regime model artifact",
    )
    ap.add_argument("--d-spatial", type=int, default=64, dest="d_spatial")
    ap.add_argument("--d-temporal", type=int, default=128, dest="d_temporal")
    ap.add_argument("--seq-len", type=int, default=64, dest="seq_len")
    ap.add_argument("--epochs", type=int, default=20)
    ap.add_argument("--folds", type=int, default=4)
    ap.add_argument("--batch-size", type=int, default=32, dest="batch_size")
    ap.add_argument("--lr", type=float, default=0.0003)
    ap.add_argument("--no-pretrain", action="store_true", dest="no_pretrain")
    ap.add_argument("--pretrain-epochs", type=int, default=5, dest="pretrain_epochs")
    ap.add_argument("--entry-bps", type=float, default=5.0, dest="entry_bps")
    ap.add_argument("--fee-bps", type=float, default=5.0, dest="fee_bps")
    ap.add_argument("--signal-horizon-idx", type=int, default=2, dest="signal_horizon_idx")
    ap.add_argument(
        "--direction-confidence-floor",
        type=float,
        default=0.55,
        dest="direction_confidence_floor",
        help="Minimum sign-aligned direction probability required to keep a signal.",
    )
    ap.add_argument(
        "--risk-ceiling",
        type=float,
        default=0.60,
        dest="risk_ceiling",
        help="Risk score above which backtest signals are zeroed.",
    )
    ap.add_argument("--primary-w-return", type=float, default=1.0, dest="primary_w_return")
    ap.add_argument("--primary-w-direction", type=float, default=0.7, dest="primary_w_direction")
    ap.add_argument("--primary-w-risk", type=float, default=0.5, dest="primary_w_risk")
    ap.add_argument("--primary-w-tc", type=float, default=0.1, dest="primary_w_tc")
    ap.add_argument(
        "--primary-selection-w-return",
        type=float,
        default=1.0,
        dest="primary_selection_w_return",
    )
    ap.add_argument(
        "--primary-selection-w-direction",
        type=float,
        default=0.9,
        dest="primary_selection_w_direction",
    )
    ap.add_argument(
        "--primary-selection-w-risk",
        type=float,
        default=0.7,
        dest="primary_selection_w_risk",
    )
    ap.add_argument(
        "--primary-selection-w-tc", type=float, default=0.1, dest="primary_selection_w_tc"
    )
    ap.add_argument("--secondary-w-return", type=float, default=0.2, dest="secondary_w_return")
    ap.add_argument(
        "--secondary-w-direction", type=float, default=1.0, dest="secondary_w_direction"
    )
    ap.add_argument("--secondary-w-risk", type=float, default=0.7, dest="secondary_w_risk")
    ap.add_argument("--secondary-w-tc", type=float, default=0.1, dest="secondary_w_tc")
    ap.add_argument(
        "--secondary-selection-w-return",
        type=float,
        default=0.25,
        dest="secondary_selection_w_return",
    )
    ap.add_argument(
        "--secondary-selection-w-direction",
        type=float,
        default=1.0,
        dest="secondary_selection_w_direction",
    )
    ap.add_argument(
        "--secondary-selection-w-risk",
        type=float,
        default=0.8,
        dest="secondary_selection_w_risk",
    )
    ap.add_argument(
        "--secondary-selection-w-tc",
        type=float,
        default=0.1,
        dest="secondary_selection_w_tc",
    )
    ap.add_argument(
        "--enable-secondary-model",
        action=argparse.BooleanOptionalAction,
        default=True,
        dest="enable_secondary_model",
        help="Train and save a smaller secondary alpha model alongside the primary model (enabled by default).",
    )
    args = ap.parse_args()
    run_pipeline(args)


if __name__ == "__main__":
    main()
