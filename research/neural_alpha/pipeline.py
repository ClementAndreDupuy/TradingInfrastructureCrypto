from __future__ import annotations

import argparse
import time
from dataclasses import asdict
from itertools import islice, product
from pathlib import Path
from typing import TYPE_CHECKING, Any, Callable, Mapping

import numpy as np
import polars as pl
import requests

from .common.symbols import binance_symbol, coinbase_symbol, kraken_symbol, okx_symbol, symbol_family
from .runtime.core_bridge import CoreBridge
from .._config import pipeline_cfg

if TYPE_CHECKING:
    from .models.trainer import TrainerConfig

_pcfg = pipeline_cfg()
N_LEVELS: int = int(_pcfg["n_levels"])
DEFAULT_EXCHANGES: list[str] = [str(exchange).upper() for exchange in _pcfg["default_exchanges"]]
BINANCE_DEPTH_URL: str = str(_pcfg["urls"]["binance_depth"])
KRAKEN_DEPTH_URL: str = str(_pcfg["urls"]["kraken_depth"])
OKX_DEPTH_URL: str = str(_pcfg["urls"]["okx_depth"])
COINBASE_DEPTH_URL: str = str(_pcfg["urls"]["coinbase_depth"])
LARGE_SELECTION_SCORE: float = float(_pcfg["large_selection_score"])
_REQUEST_TIMEOUT_S: int = int(_pcfg["request_timeout_s"])
_HOLDOUT_FRAC: float = float(_pcfg["holdout_frac"])

def _parse_exchange_list(raw_value: str) -> list[str]:
    return [exchange.strip().upper() for exchange in raw_value.split(",") if exchange.strip()]

def _with_row_levels(
    *,
    exchange: str,
    symbol: str,
    bids: list[tuple[float, float]],
    asks: list[tuple[float, float]],
) -> dict[str, Any] | None:
    if not bids or not asks:
        return None
    row: dict[str, Any] = {
        "timestamp_ns": time.time_ns(),
        "exchange": exchange,
        "symbol": symbol,
        "best_bid": bids[0][0],
        "best_ask": asks[0][0],
    }
    for index, (price, size) in enumerate(bids[:N_LEVELS], start=1):
        row[f"bid_price_{index}"] = price
        row[f"bid_size_{index}"] = size
    for index, (price, size) in enumerate(asks[:N_LEVELS], start=1):
        row[f"ask_price_{index}"] = price
        row[f"ask_size_{index}"] = size
    return row

def _request_json(url: str, *, params: Mapping[str, Any]) -> Any:
    response = requests.get(url, params=params, timeout=_REQUEST_TIMEOUT_S)
    response.raise_for_status()
    return response.json()

def _fetch_binance_l5(symbol: str) -> dict[str, Any] | None:
    try:
        venue_symbol = binance_symbol(symbol)
        payload = _request_json(BINANCE_DEPTH_URL, params={"symbol": venue_symbol, "limit": N_LEVELS})
        bids = [(float(price), float(size)) for price, size in payload["bids"][:N_LEVELS]]
        asks = [(float(price), float(size)) for price, size in payload["asks"][:N_LEVELS]]
        return _with_row_levels(exchange="BINANCE", symbol=venue_symbol, bids=bids, asks=asks)
    except Exception as exc:
        print(f"  [WARN] Binance L5 fetch: {exc}")
        return None

def _fetch_kraken_l5(symbol: str) -> dict[str, Any] | None:
    try:
        pair = kraken_symbol(symbol)
        payload = _request_json(KRAKEN_DEPTH_URL, params={"pair": pair, "count": N_LEVELS})
        book = next(iter(payload["result"].values()))
        bids = [(float(price), float(size)) for price, size, *_ in book["bids"][:N_LEVELS]]
        asks = [(float(price), float(size)) for price, size, *_ in book["asks"][:N_LEVELS]]
        return _with_row_levels(exchange="KRAKEN", symbol=pair, bids=bids, asks=asks)
    except Exception as exc:
        print(f"  [WARN] Kraken L5 fetch: {exc}")
        return None

def _fetch_okx_l5(symbol: str) -> dict[str, Any] | None:
    try:
        venue_symbol = okx_symbol(symbol)
        payload = _request_json(OKX_DEPTH_URL, params={"instId": venue_symbol, "sz": N_LEVELS})
        books = payload.get("data", [])
        if not books:
            return None
        book = books[0]
        bids = [(float(price), float(size)) for price, size, *_ in book.get("bids", [])[:N_LEVELS]]
        asks = [(float(price), float(size)) for price, size, *_ in book.get("asks", [])[:N_LEVELS]]
        return _with_row_levels(exchange="OKX", symbol=venue_symbol, bids=bids, asks=asks)
    except Exception as exc:
        print(f"  [WARN] OKX L5 fetch: {exc}")
        return None

def _fetch_coinbase_l5(symbol: str) -> dict[str, Any] | None:
    try:
        product_id = coinbase_symbol(symbol)
        payload = _request_json(COINBASE_DEPTH_URL, params={"product_id": product_id, "limit": N_LEVELS})
        pricebook = payload.get("pricebook", {})
        bids = [(float(level["price"]), float(level["size"])) for level in pricebook.get("bids", [])[:N_LEVELS]]
        asks = [(float(level["price"]), float(level["size"])) for level in pricebook.get("asks", [])[:N_LEVELS]]
        return _with_row_levels(exchange="COINBASE", symbol=product_id, bids=bids, asks=asks)
    except Exception as exc:
        print(f"  [WARN] Coinbase L5 fetch: {exc}")
        return None

FETCHERS: dict[str, Callable[[str], dict[str, Any] | None]] = {
    "BINANCE": _fetch_binance_l5,
    "KRAKEN": _fetch_kraken_l5,
    "OKX": _fetch_okx_l5,
    "COINBASE": _fetch_coinbase_l5,
}

_TRADE_FLOW_DEFAULTS: dict[str, Any] = {
    "last_trade_price": 0.0,
    "last_trade_size": 0.0,
    "recent_traded_volume": 0.0,
    "trade_direction": 255,
}
_TRADE_COLUMNS: tuple[str, ...] = (
    "last_trade_price",
    "last_trade_size",
    "recent_traded_volume",
    "trade_direction",
)


def _ensure_trade_flow_columns(df: pl.DataFrame) -> pl.DataFrame:
    if len(df) == 0:
        return df
    out = df
    for column, default in _TRADE_FLOW_DEFAULTS.items():
        if column not in out.columns:
            out = out.with_columns(pl.lit(default).alias(column))
    return out

def _has_dense_levels(df: pl.DataFrame) -> bool:
    if "bid_prices" in df.columns and "ask_prices" in df.columns:
        return True
    level_columns: list[str] = []
    for side in ("bid", "ask"):
        for field in ("price", "size"):
            level_columns.extend([f"{side}_{field}_{idx}" for idx in range(1, N_LEVELS + 1)])
    return all(column in df.columns for column in level_columns)

def _count_columns_present(df: pl.DataFrame) -> tuple[list[str], list[str]]:
    bid_candidates = [f"bid_oc_{idx}" for idx in range(1, N_LEVELS + 1)] + [f"bid_order_count_{idx}" for idx in range(1, N_LEVELS + 1)]
    ask_candidates = [f"ask_oc_{idx}" for idx in range(1, N_LEVELS + 1)] + [f"ask_order_count_{idx}" for idx in range(1, N_LEVELS + 1)]
    bid_found = [column for column in bid_candidates if column in df.columns]
    ask_found = [column for column in ask_candidates if column in df.columns]
    return bid_found, ask_found

def _validate_alpha_input_schema(df: pl.DataFrame) -> None:
    if not _has_dense_levels(df):
        raise RuntimeError(
            f"Dataset must include {N_LEVELS} levels for bid/ask price+size columns (or list-level bid_prices/ask_prices)."
        )
    missing_trade = [column for column in _TRADE_COLUMNS if column not in df.columns]
    if missing_trade:
        raise RuntimeError(f"Dataset missing trade-flow columns required by both models: {missing_trade}")
    bid_count_columns, ask_count_columns = _count_columns_present(df)
    if not bid_count_columns or not ask_count_columns:
        raise RuntimeError(
            "Dataset missing per-level order-count columns required for count-aware depth features (bid_oc_* or bid_order_count_*, ask_oc_* or ask_order_count_*)."
        )
    non_zero_bid = int((df.select([pl.col(column).abs().sum().alias(column) for column in bid_count_columns]).to_numpy() > 0).sum())
    non_zero_ask = int((df.select([pl.col(column).abs().sum().alias(column) for column in ask_count_columns]).to_numpy() > 0).sum())
    if non_zero_bid == 0 or non_zero_ask == 0:
        raise RuntimeError("Order-count columns are present but contain only zeros; cannot train count-aware alpha models.")
    print(
        f"[DATA] alpha schema verified: levels={N_LEVELS} trade_cols={len(_TRADE_COLUMNS)} count_cols_bid={len(bid_count_columns)} count_cols_ask={len(ask_count_columns)}"
    )

def collect_from_core_bridge(n_ticks: int, interval_ms: int) -> pl.DataFrame | None:
    bridge = CoreBridge()
    if not bridge.open():
        return None
    rows: list[dict[str, Any]] = []
    interval_s = interval_ms / 1000.0
    try:
        while len(rows) < n_ticks:
            rows.extend(bridge.read_new_ticks())
            if len(rows) < n_ticks:
                time.sleep(interval_s)
    finally:
        bridge.close()
    if not rows:
        return None
    return _ensure_trade_flow_columns(pl.DataFrame(rows[:n_ticks]).sort("timestamp_ns"))

def _enforce_symbol_and_exchange_coverage(
    df: pl.DataFrame,
    exchanges: list[str],
    symbol: str,
) -> pl.DataFrame:
    wanted_symbol_family = symbol_family(symbol)
    wanted_exchanges = {exchange.upper() for exchange in exchanges}
    filtered_rows = [
        row
        for row in df.to_dicts()
        if row.get("exchange", "").upper() in wanted_exchanges
        and symbol_family(str(row.get("symbol", ""))) == wanted_symbol_family
    ]
    if not filtered_rows:
        return pl.DataFrame([])
    filtered_df = pl.DataFrame(filtered_rows).sort("timestamp_ns")
    found_exchanges = {exchange.upper() for exchange in filtered_df["exchange"].to_list()}
    missing_exchanges = sorted(wanted_exchanges - found_exchanges)
    if missing_exchanges:
        raise RuntimeError(
            "Dataset does not include all configured exchanges for requested symbol "
            f"{symbol}: missing={missing_exchanges}, found={sorted(found_exchanges)}"
        )
    return filtered_df

def _collect_l5_ticks_rest(
    n_ticks: int,
    interval_ms: int,
    exchanges: list[str],
    symbol: str,
) -> pl.DataFrame:
    rows: list[dict[str, Any]] = []
    interval_s = interval_ms / 1000.0
    print(f"Collecting {n_ticks} L5 snapshots per exchange over REST (interval {interval_ms} ms)…")
    for tick_index in range(n_ticks):
        for exchange in exchanges:
            fetcher = FETCHERS.get(exchange)
            if fetcher is None:
                print(f"  [WARN] Unknown exchange: {exchange}")
                continue
            row = fetcher(symbol)
            if row is not None:
                rows.append(row)
        if tick_index < n_ticks - 1:
            time.sleep(interval_s)
    if not rows:
        raise RuntimeError("No data collected — check network and exchange APIs.")
    return _ensure_trade_flow_columns(pl.DataFrame(rows).sort("timestamp_ns"))

def collect_l5_ticks(
    n_ticks: int,
    interval_ms: int,
    exchanges: list[str] | None = None,
    allow_rest_fallback: bool = True,
    symbol: str = "BTCUSDT",
) -> pl.DataFrame:
    effective_exchanges = exchanges or DEFAULT_EXCHANGES
    bridge_df = collect_from_core_bridge(n_ticks=n_ticks, interval_ms=interval_ms)
    if bridge_df is not None and len(bridge_df) >= n_ticks // 2:
        bridge_df = _enforce_symbol_and_exchange_coverage(bridge_df, effective_exchanges, symbol)
        if len(bridge_df) >= n_ticks:
            return bridge_df
        if not allow_rest_fallback:
            raise RuntimeError(
                f"Core bridge returned {len(bridge_df)} ticks (< {n_ticks}) and REST fallback is disabled."
            )
        print(f"[WARN] bridge returned {len(bridge_df)} ticks (< {n_ticks}); topping up with REST")
        rest_df = _collect_l5_ticks_rest(
            n_ticks=n_ticks - len(bridge_df),
            interval_ms=interval_ms,
            exchanges=effective_exchanges,
            symbol=symbol,
        )
        return _enforce_symbol_and_exchange_coverage(
            pl.concat([bridge_df, rest_df]).sort("timestamp_ns"),
            effective_exchanges,
            symbol,
        )
    if not allow_rest_fallback:
        raise RuntimeError("Core bridge unavailable and REST fallback disabled.")
    print("[WARN] core bridge unavailable or insufficient; falling back to REST collection")
    rest_df = _collect_l5_ticks_rest(n_ticks, interval_ms, effective_exchanges, symbol)
    return _enforce_symbol_and_exchange_coverage(rest_df, effective_exchanges, symbol)

def _fold_slices(total_ticks: int, n_folds: int, train_frac: float) -> list[tuple[int, int]]:
    fold_size = total_ticks // n_folds
    return [
        (int((index + 1) * fold_size - fold_size * (1 - train_frac)), (index + 1) * fold_size)
        for index in range(n_folds)
    ]

def _evaluate_state_on_holdout(df: pl.DataFrame, state_dict: dict[str, Any], cfg: TrainerConfig) -> float:
    import torch
    from torch.utils.data import DataLoader

    from .data.dataset import DatasetConfig, LOBDataset
    from .models.model import CryptoAlphaNet
    from .models.trainer import selection_score

    dataset = LOBDataset(df, DatasetConfig(seq_len=cfg.seq_len))
    if len(dataset) == 0:
        return float("inf")
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    model = CryptoAlphaNet(
        d_spatial=cfg.d_spatial,
        d_temporal=cfg.d_temporal,
        n_lob_heads=cfg.n_lob_heads,
        n_lob_layers=cfg.n_lob_layers,
        n_temp_heads=cfg.n_temp_heads,
        n_temp_layers=cfg.n_temp_layers,
        dropout=cfg.dropout,
        seq_len=cfg.seq_len,
    ).to(device)
    model.eval()
    model.load_state_dict(state_dict, strict=False)
    loader = DataLoader(dataset, batch_size=64, shuffle=False)
    totals = {"loss_return": 0.0, "loss_direction": 0.0, "loss_risk": 0.0, "loss_tc": 0.0}
    batch_count = 0
    with torch.no_grad():
        for batch in loader:
            labels = batch["labels"].to(device)
            predictions = model(batch["lob"].to(device), batch["scalar"].to(device))
            return_diff = predictions["returns"][:, -1, :] - labels[:, -1, :4]
            totals["loss_return"] += float((return_diff * return_diff).mean().item())
            totals["loss_direction"] += float(
                torch.nn.functional.cross_entropy(
                    predictions["direction"][:, -1, :],
                    labels[:, -1, 4].long(),
                ).item()
            )
            totals["loss_risk"] += float(
                torch.nn.functional.binary_cross_entropy(
                    predictions["risk"][:, -1],
                    labels[:, -1, 5],
                ).item()
            )
            predicted_mid_return = predictions["returns"][:, -1, 2]
            totals["loss_tc"] += float(
                torch.nn.functional.relu(cfg.tc_bps * 0.0001 - predicted_mid_return.abs()).mean().item()
            )
            batch_count += 1
    average_metrics = {name: value / max(batch_count, 1) for name, value in totals.items()}
    return selection_score(average_metrics, cfg)

def _atomic_torch_save(state_dict: dict[str, Any], output_path: Path) -> None:
    import torch

    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = output_path.with_suffix(f"{output_path.suffix}.tmp")
    torch.save(state_dict, temporary_path)
    temporary_path.replace(output_path)

def _blend_fold_results(primary_folds: list[dict[str, Any]], secondary_folds: list[dict[str, Any]]) -> list[dict[str, Any]]:
    if len(primary_folds) != len(secondary_folds):
        raise ValueError("Primary/secondary fold counts do not match for ensembling.")
    blended_folds: list[dict[str, Any]] = []
    for primary_fold, secondary_fold in zip(primary_folds, secondary_folds):
        primary_predictions = primary_fold.get("predictions")
        secondary_predictions = secondary_fold.get("predictions")
        primary_direction = primary_fold.get("direction_probs")
        secondary_direction = secondary_fold.get("direction_probs")
        primary_risk = primary_fold.get("risk_scores")
        secondary_risk = secondary_fold.get("risk_scores")
        primary_labels = primary_fold.get("labels")
        secondary_labels = secondary_fold.get("labels")
        tensors = [
            primary_predictions,
            secondary_predictions,
            primary_direction,
            secondary_direction,
            primary_risk,
            secondary_risk,
            primary_labels,
            secondary_labels,
        ]
        if any(tensor is None for tensor in tensors):
            raise ValueError("Missing predictions or labels while building fold ensemble.")
        if (
            primary_predictions.shape != secondary_predictions.shape
            or primary_direction.shape != secondary_direction.shape
            or primary_risk.shape != secondary_risk.shape
            or primary_labels.shape != secondary_labels.shape
        ):
            raise ValueError("Primary/secondary fold tensor shapes do not match.")
        blended_folds.append(
            {
                **primary_fold,
                "predictions": ((primary_predictions + secondary_predictions) / 2.0).astype(np.float32),
                "direction_probs": ((primary_direction + secondary_direction) / 2.0).astype(np.float32),
                "risk_scores": ((primary_risk + secondary_risk) / 2.0).astype(np.float32),
                "labels": primary_labels,
                "ensemble": True,
            }
        )
    return blended_folds

def _signal_from_fold_outputs(
    fold: dict[str, Any],
    *,
    horizon_idx: int,
    direction_confidence_floor: float,
    risk_ceiling: float,
) -> np.ndarray:
    signal = np.asarray(fold["predictions"][:, horizon_idx], dtype=np.float32).copy()
    direction_probs = fold.get("direction_probs")
    if direction_probs is not None:
        direction_array = np.asarray(direction_probs, dtype=np.float32)
        if direction_array.ndim == 2 and direction_array.shape[1] >= 3:
            long_confidence = direction_array[:, 2]
            short_confidence = direction_array[:, 0]
            direction_mask = (
                ((signal > 0.0) & (long_confidence < direction_confidence_floor))
                | ((signal < 0.0) & (short_confidence < direction_confidence_floor))
            )
            signal[direction_mask] = 0.0
    risk_scores = fold.get("risk_scores")
    if risk_scores is not None:
        risk_array = np.asarray(risk_scores, dtype=np.float32)
        signal *= np.clip(1.0 - risk_array, 0.0, 1.0)
        signal[risk_array > risk_ceiling] = 0.0
    return signal.astype(np.float32)

def _trainer_config_from_args(args: argparse.Namespace, *, secondary: bool = False) -> TrainerConfig:
    from .models.trainer import TrainerConfig

    prefix = "secondary" if secondary else "primary"
    kwargs: dict[str, Any] = {
        "epochs": args.epochs,
        "n_folds": args.folds,
        "pretrain": not args.no_pretrain,
        "pretrain_epochs": args.pretrain_epochs,
        "seq_len": args.seq_len,
        "batch_size": args.batch_size,
        "lr": args.lr,
        "w_return": getattr(args, f"{prefix}_w_return"),
        "w_direction": getattr(args, f"{prefix}_w_direction"),
        "w_risk": getattr(args, f"{prefix}_w_risk"),
        "w_tc": getattr(args, f"{prefix}_w_tc"),
        "selection_w_return": getattr(args, f"{prefix}_selection_w_return"),
        "selection_w_direction": getattr(args, f"{prefix}_selection_w_direction"),
        "selection_w_risk": getattr(args, f"{prefix}_selection_w_risk"),
        "selection_w_tc": getattr(args, f"{prefix}_selection_w_tc"),
    }
    if secondary:
        from .._config import model_cfg as _model_cfg
        _scfg = _model_cfg()["secondary"]
        kwargs.update({"d_spatial": _scfg["d_spatial"], "d_temporal": _scfg["d_temporal"], "n_temp_layers": _scfg["n_temp_layers"]})
    else:
        kwargs.update({"d_spatial": args.d_spatial, "d_temporal": args.d_temporal})
    return TrainerConfig(**kwargs)

def _mean_selection_score(folds: list[dict[str, Any]]) -> float:
    if not folds:
        return LARGE_SELECTION_SCORE
    scores = [_selection_score(fold) for fold in folds]
    if not scores:
        return LARGE_SELECTION_SCORE
    return float(np.mean(scores))


def _selection_score(fold: Mapping[str, Any]) -> float:
    raw_score = fold.get("best_selection_score", LARGE_SELECTION_SCORE)
    try:
        return float(raw_score)
    except (TypeError, ValueError) as exc:
        raise RuntimeError(f"best_selection_score must be numeric, got {raw_score!r}") from exc

def _candidate_dicts(search_space: dict[str, list[Any]], max_trials: int) -> list[dict[str, Any]]:
    keys = [key for key, values in search_space.items() if values]
    if not keys:
        return []
    values = [search_space[key] for key in keys]
    combos = [dict(zip(keys, combo)) for combo in islice(product(*values), max_trials)]
    return combos

def _auto_tune_trainer_config(
    df: pl.DataFrame,
    base_cfg: TrainerConfig,
    *,
    secondary: bool,
    enabled: bool,
) -> TrainerConfig:
    from .models.trainer import TrainerConfig, walk_forward_train
    from .._config import model_cfg as _model_cfg

    if not enabled:
        return base_cfg
    search_cfg = _model_cfg().get("search", {})
    if not bool(search_cfg.get("enabled", True)):
        return base_cfg
    model_key = "secondary" if secondary else "primary"
    search_space = search_cfg.get(model_key, {})
    max_trials = int(search_cfg.get("max_trials", 0))
    if max_trials <= 0:
        return base_cfg
    trials = _candidate_dicts(search_space, max_trials)
    if not trials:
        return base_cfg
    tuning_epochs = int(search_cfg.get("tuning_epochs", base_cfg.epochs))
    best_cfg = base_cfg
    best_score = LARGE_SELECTION_SCORE
    model_name = "secondary" if secondary else "primary"
    print(f"[HPO] tuning {model_name} model over {len(trials)} trials")
    for trial_idx, trial in enumerate(trials, start=1):
        cfg_kwargs = asdict(base_cfg)
        cfg_kwargs.update(trial)
        cfg_kwargs["epochs"] = max(1, min(base_cfg.epochs, tuning_epochs))
        cfg_kwargs["verbose"] = False
        candidate_cfg = TrainerConfig(**cfg_kwargs)
        folds = walk_forward_train(df, candidate_cfg)
        score = _mean_selection_score(folds)
        print(f"[HPO] {model_name} trial {trial_idx}/{len(trials)} score={score:.6f} params={trial}")
        if score < best_score:
            best_score = score
            best_cfg = TrainerConfig(**asdict(candidate_cfg) | {"verbose": base_cfg.verbose, "epochs": base_cfg.epochs})
    print(f"[HPO] selected {model_name} score={best_score:.6f}")
    return best_cfg

def _load_or_collect_data(args: argparse.Namespace) -> pl.DataFrame:
    if args.data_path and Path(args.data_path).exists():
        print(f"Loading data from {args.data_path}")
        return pl.read_parquet(args.data_path)
    return collect_l5_ticks(
        n_ticks=args.ticks,
        interval_ms=args.interval_ms,
        exchanges=_parse_exchange_list(args.exchanges),
        symbol=args.symbol,
        allow_rest_fallback=not args.core_only,
    )

def _save_data_if_requested(df: pl.DataFrame, save_path: str | None) -> None:
    if save_path is None:
        return
    output_path = Path(save_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    df.write_parquet(str(output_path))
    print(f"Data saved → {output_path}")

def _build_tick_signals(signals: np.ndarray, test_length: int, seq_len: int) -> np.ndarray:
    tick_signals = np.zeros(test_length, dtype=np.float32)
    for index, signal in enumerate(signals):
        tick_index = min(index + seq_len - 1, test_length - 1)
        tick_signals[tick_index] = signal
    return tick_signals

def _run_backtests(
    df: pl.DataFrame,
    fold_results: list[dict[str, Any]],
    trainer_cfg: TrainerConfig,
    args: argparse.Namespace,
) -> dict[str, Any]:
    from .evaluation.backtest import BacktestConfig, NeuralAlphaBacktest

    backtest_cfg = BacktestConfig(entry_threshold_bps=args.entry_bps, taker_fee_bps=args.fee_bps)
    fold_metrics: list[dict[str, Any]] = []
    for fold_result, (start, end) in zip(
        fold_results,
        _fold_slices(len(df), args.folds, trainer_cfg.train_frac),
    ):
        test_df = df[start:end]
        signals = _signal_from_fold_outputs(
            fold_result,
            horizon_idx=args.signal_horizon_idx,
            direction_confidence_floor=args.direction_confidence_floor,
            risk_ceiling=args.risk_ceiling,
        )
        backtest = NeuralAlphaBacktest(backtest_cfg)
        result = backtest.run(
            test_df,
            _build_tick_signals(signals, len(test_df), trainer_cfg.seq_len),
        )
        fold_metrics.append(result["metrics"])
        print(f"Fold {fold_result['fold']} backtest: {result['metrics']}")
    merged_metrics: dict[str, Any] = {
        "total_trades": sum(metrics.get("total_trades", 0) for metrics in fold_metrics)
    }
    for key in fold_metrics[0]:
        if isinstance(fold_metrics[0][key], float):
            values = [metrics[key] for metrics in fold_metrics if key in metrics]
            merged_metrics[key] = float(np.mean(values)) if values else 0.0
    return merged_metrics

def _print_alpha_diagnostics(fold_results: list[dict[str, Any]], backtest_metrics: dict[str, Any], args: argparse.Namespace) -> None:
    from .evaluation.alpha_regression import (
        analyse_alpha,
        analyse_direction_calibration,
        analyse_flat_threshold_sensitivity,
        print_alpha_report,
        print_direction_diagnostics,
        sweep_direction_thresholds,
    )

    alpha_metrics = analyse_alpha(fold_results, horizon_idx=2)
    calibration = analyse_direction_calibration(fold_results, horizon_idx=args.signal_horizon_idx)
    threshold_sweep = sweep_direction_thresholds(fold_results, horizon_idx=args.signal_horizon_idx)
    flat_sensitivity = analyse_flat_threshold_sensitivity(fold_results, horizon_idx=args.signal_horizon_idx)
    print_alpha_report(alpha_metrics, backtest_metrics)
    print_direction_diagnostics(calibration, threshold_sweep, flat_sensitivity)

def _train_regime_model_if_possible(args: argparse.Namespace) -> None:
    from research.regime import RegimeConfig, save_regime_artifact, train_regime_model_from_ipc

    try:
        regime_cfg = RegimeConfig(n_regimes=args.regimes)
        regime_artifact, regime_distribution = train_regime_model_from_ipc(args.ipc_dir, regime_cfg)
        save_regime_artifact(regime_artifact, args.save_regime_model)
        print(
            f"R2 regime model trained (n_regimes={regime_cfg.n_regimes}) from {args.ipc_dir} and saved → {args.save_regime_model}"
        )
        print(f"R2 regime distribution: {regime_distribution}")
    except Exception as exc:
        print(f"[WARN] R2 regime training skipped: {exc}")

def _best_fold(fold_results: list[dict[str, Any]]) -> dict[str, Any]:
    return min(fold_results, key=_selection_score)

def _holdout_df(df: pl.DataFrame) -> pl.DataFrame:
    return df[int(len(df) * _HOLDOUT_FRAC) :]

def _select_primary_state(
    df: pl.DataFrame,
    output_path: Path,
    trainer_cfg: TrainerConfig,
    fold_results: list[dict[str, Any]],
) -> tuple[dict[str, Any], float | None, float | None, str]:
    import torch

    challenger_state = _best_fold(fold_results)["model_state"]
    selected_state = challenger_state
    selected_name = "challenger"
    holdout_df = _holdout_df(df)
    if len(holdout_df) < trainer_cfg.seq_len * 2:
        return selected_state, None, None, selected_name
    challenger_score = _evaluate_state_on_holdout(holdout_df, challenger_state, trainer_cfg)
    incumbent_score: float | None = None
    if output_path.exists():
        incumbent_state = torch.load(output_path, map_location="cpu", weights_only=True)
        incumbent_score = _evaluate_state_on_holdout(holdout_df, incumbent_state, trainer_cfg)
        if incumbent_score < challenger_score:
            selected_state = incumbent_state
            selected_name = "incumbent"
            print(
                f"[MODEL_SELECT] challenger rejected on holdout (incumbent_mse={incumbent_score:.6e}, challenger_mse={challenger_score:.6e})"
            )
    return selected_state, incumbent_score, challenger_score, selected_name

def _save_models(
    df: pl.DataFrame,
    args: argparse.Namespace,
    primary_cfg: TrainerConfig,
    primary_folds: list[dict[str, Any]],
    secondary_folds: list[dict[str, Any]] | None,
) -> None:
    if args.save_model is None:
        return
    primary_path = Path(args.save_model)
    selected_state, incumbent_score, challenger_score, selected_name = _select_primary_state(
        df,
        primary_path,
        primary_cfg,
        primary_folds,
    )
    _atomic_torch_save(selected_state, primary_path)
    print(f"Best primary model saved → {primary_path}")
    if challenger_score is not None:
        print(
            f"[MODEL_SELECT] holdout mid-return MSE incumbent={incumbent_score} challenger={challenger_score} selected={selected_name}"
        )
    if not args.enable_secondary_model or not secondary_folds:
        return
    secondary_cfg = _trainer_config_from_args(args, secondary=True)
    secondary_path = primary_path.with_name(f"{primary_path.stem}_secondary{primary_path.suffix}")
    secondary_state = _best_fold(secondary_folds)["model_state"]
    _atomic_torch_save(secondary_state, secondary_path)
    holdout_df = _holdout_df(df)
    secondary_score = None
    if len(holdout_df) >= secondary_cfg.seq_len * 2:
        secondary_score = _evaluate_state_on_holdout(holdout_df, secondary_state, secondary_cfg)
    suffix = f" (holdout_mse={secondary_score:.6e})" if secondary_score is not None else ""
    print(f"Secondary ensemble model saved → {secondary_path}{suffix}")

def run_pipeline(args: argparse.Namespace) -> None:
    from .models.trainer import walk_forward_train

    df = _load_or_collect_data(args)
    _save_data_if_requested(df, args.save_data)
    _validate_alpha_input_schema(df)
    print(f"Dataset: {len(df)} ticks  columns={df.columns[:8]}…\n")
    primary_cfg = _auto_tune_trainer_config(
        df,
        _trainer_config_from_args(args),
        secondary=False,
        enabled=args.auto_tune_hparams,
    )
    primary_folds = walk_forward_train(df, primary_cfg)
    if not primary_folds:
        print("No fold results — dataset too small for the chosen seq_len / n_folds.")
        return
    effective_folds = primary_folds
    secondary_folds: list[dict[str, Any]] | None = None
    if args.enable_secondary_model:
        print("Training secondary ensemble model (d_spatial=32, d_temporal=64, n_temp_layers=1)…")
        secondary_cfg = _auto_tune_trainer_config(
            df,
            _trainer_config_from_args(args, secondary=True),
            secondary=True,
            enabled=args.auto_tune_hparams,
        )
        secondary_folds = walk_forward_train(df, secondary_cfg)
        if not secondary_folds:
            raise RuntimeError("Secondary ensemble model produced no fold results.")
        effective_folds = _blend_fold_results(primary_folds, secondary_folds)
        print("Secondary model blended with primary predictions (50/50).")
    backtest_metrics = _run_backtests(df, effective_folds, primary_cfg, args)
    _print_alpha_diagnostics(effective_folds, backtest_metrics, args)
    _train_regime_model_if_possible(args)
    _save_models(df, args, primary_cfg, primary_folds, secondary_folds)

def build_argument_parser() -> argparse.ArgumentParser:
    _col = _pcfg["collection"]
    _bt = _pcfg["backtest"]
    _tr = _pcfg["training"]
    from .._config import model_cfg as _model_cfg
    _mcfg = _model_cfg()["trainer"]
    parser = argparse.ArgumentParser(description="Neural crypto alpha pipeline")
    parser.add_argument("--ticks", type=int, default=_col["ticks"])
    parser.add_argument("--interval-ms", type=int, default=_col["interval_ms"], dest="interval_ms")
    parser.add_argument("--symbol", type=str, default=_col["symbol"])
    parser.add_argument("--exchanges", type=str, default=",".join(DEFAULT_EXCHANGES))
    parser.add_argument("--core-only", action="store_true")
    parser.add_argument("--data-path", type=str, default=None, dest="data_path")
    parser.add_argument("--save-data", type=str, default=None, dest="save_data")
    parser.add_argument("--save-model", type=str, default=None, dest="save_model")
    parser.add_argument("--ipc-dir", type=str, default=_col["ipc_dir"], dest="ipc_dir")
    parser.add_argument("--regimes", type=int, default=_tr["regimes"])
    parser.add_argument("--save-regime-model", type=str, default=_tr["save_regime_model"], dest="save_regime_model")
    parser.add_argument("--d-spatial", type=int, default=_mcfg["d_spatial"], dest="d_spatial")
    parser.add_argument("--d-temporal", type=int, default=_mcfg["d_temporal"], dest="d_temporal")
    parser.add_argument("--seq-len", type=int, default=_mcfg["seq_len"], dest="seq_len")
    parser.add_argument("--epochs", type=int, default=_mcfg["epochs"])
    parser.add_argument("--folds", type=int, default=_mcfg["n_folds"])
    parser.add_argument("--batch-size", type=int, default=_mcfg["batch_size"], dest="batch_size")
    parser.add_argument("--lr", type=float, default=_mcfg["lr"])
    parser.add_argument("--no-pretrain", action="store_true", dest="no_pretrain")
    parser.add_argument("--pretrain-epochs", type=int, default=_mcfg["pretrain_epochs"], dest="pretrain_epochs")
    parser.add_argument("--entry-bps", type=float, default=_bt["entry_bps"], dest="entry_bps")
    parser.add_argument("--fee-bps", type=float, default=_bt["fee_bps"], dest="fee_bps")
    parser.add_argument("--signal-horizon-idx", type=int, default=_bt["signal_horizon_idx"], dest="signal_horizon_idx")
    parser.add_argument("--direction-confidence-floor", type=float, default=_bt["direction_confidence_floor"], dest="direction_confidence_floor")
    parser.add_argument("--risk-ceiling", type=float, default=_bt["risk_ceiling"], dest="risk_ceiling")
    for prefix in ("primary", "secondary"):
        w = _pcfg["weights"][prefix]
        parser.add_argument(f"--{prefix}-w-return", type=float, default=w["w_return"], dest=f"{prefix}_w_return")
        parser.add_argument(f"--{prefix}-w-direction", type=float, default=w["w_direction"], dest=f"{prefix}_w_direction")
        parser.add_argument(f"--{prefix}-w-risk", type=float, default=w["w_risk"], dest=f"{prefix}_w_risk")
        parser.add_argument(f"--{prefix}-w-tc", type=float, default=w["w_tc"], dest=f"{prefix}_w_tc")
        parser.add_argument(f"--{prefix}-selection-w-return", type=float, default=w["selection_w_return"], dest=f"{prefix}_selection_w_return")
        parser.add_argument(f"--{prefix}-selection-w-direction", type=float, default=w["selection_w_direction"], dest=f"{prefix}_selection_w_direction")
        parser.add_argument(f"--{prefix}-selection-w-risk", type=float, default=w["selection_w_risk"], dest=f"{prefix}_selection_w_risk")
        parser.add_argument(f"--{prefix}-selection-w-tc", type=float, default=w["selection_w_tc"], dest=f"{prefix}_selection_w_tc")
    parser.add_argument(
        "--enable-secondary-model",
        action=argparse.BooleanOptionalAction,
        default=True,
        dest="enable_secondary_model",
        help="Train and save a smaller secondary alpha model alongside the primary model (enabled by default).",
    )
    parser.add_argument(
        "--auto-tune-hparams",
        action=argparse.BooleanOptionalAction,
        default=True,
        dest="auto_tune_hparams",
    )
    return parser

def main() -> None:
    run_pipeline(build_argument_parser().parse_args())

if __name__ == "__main__":
    main()
