"""
Daily model retraining job for CryptoAlphaNet.

Run via cron at 00:30 UTC (markets quietest, data from previous day settled):
    30 0 * * * /path/to/venv/bin/python /path/to/deploy/daily_train.py

What it does:
    1. Collects a fresh window of L5 LOB ticks for a target symbol.
    2. Trains CryptoAlphaNet with walk-forward cross-validation.
    3. Compares the new model against the current production model.
    4. Promotes the new model only if it beats current on held-out data.
    5. Writes metrics to logs/train_<date>.json for Prometheus pickup.

Environment variables:
    MODEL_DIR        Directory for model checkpoints (default: models/)
    DATA_DIR         Directory for tick data cache (default: data/)
    TRAIN_TICKS      Ticks to collect per exchange (default: 2000)
    TRAIN_INTERVAL_MS Polling interval while collecting (default: 500)
    EPOCHS           Training epochs per fold (default: 30)
    PROMOTE_IC_MIN   Min IC on held-out data to promote (default: 0.02)
    TRAIN_SYMBOL     Symbol to train (default: SOLUSDT)
"""
from __future__ import annotations

import json
import logging
import os
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
import torch

ROOT = Path(__file__).parent.parent
sys.path.insert(0, str(ROOT))

from research.neural_alpha.alpha_regression import analyse_alpha
from research.neural_alpha.backtest import BacktestConfig, NeuralAlphaBacktest
from research.neural_alpha.features import compute_lob_tensor, compute_scalar_features, normalise_scalar
from research.neural_alpha.governance import ChampionChallengerRegistry
from research.neural_alpha.model import CryptoAlphaNet
from research.neural_alpha.pipeline import _blend_fold_results, collect_l5_ticks
from research.neural_alpha.trainer import TrainerConfig, walk_forward_train

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s",
    handlers=[logging.StreamHandler()],
)
log = logging.getLogger("daily_train")

MODEL_DIR = Path(os.getenv("MODEL_DIR", ROOT / "models"))
DATA_DIR = Path(os.getenv("DATA_DIR", ROOT / "data"))
LOG_DIR = ROOT / "logs"

TRAIN_TICKS = int(os.getenv("TRAIN_TICKS", "2000"))
TRAIN_INTERVAL_MS = int(os.getenv("TRAIN_INTERVAL_MS", "500"))
EPOCHS = int(os.getenv("EPOCHS", "30"))
PROMOTE_IC_MIN = float(os.getenv("PROMOTE_IC_MIN", "0.02"))
TRAIN_SYMBOL = os.getenv("TRAIN_SYMBOL", "SOLUSDT")
_SYMBOL_TAG = TRAIN_SYMBOL.lower()

PROD_MODEL_PATH = MODEL_DIR / f"neural_alpha_{_SYMBOL_TAG}_latest.pt"
CANDIDATE_MODEL_PATH = MODEL_DIR / f"neural_alpha_{_SYMBOL_TAG}_candidate.pt"
SECONDARY_MODEL_PATH = MODEL_DIR / f"neural_alpha_{_SYMBOL_TAG}_secondary.pt"
NORM_STATS_PATH = MODEL_DIR / "scalar_norm_stats.npz"
REGISTRY_PATH = MODEL_DIR / "model_registry.json"


def _ensure_dirs() -> None:
    for d in (MODEL_DIR, DATA_DIR, LOG_DIR):
        d.mkdir(parents=True, exist_ok=True)


def _load_prod_model_ic() -> float:
    """Read the IC of the current production model from its metadata file."""
    meta_path = PROD_MODEL_PATH.with_suffix(".json")
    if not meta_path.exists():
        return -1.0
    with open(meta_path) as f:
        meta = json.load(f)
    return float(meta.get("ic_mean", -1.0))


def _save_model_meta(path: Path, metrics: dict) -> None:
    with open(path.with_suffix(".json"), "w") as f:
        json.dump(metrics, f, indent=2)


def _promote_candidate() -> None:
    CANDIDATE_MODEL_PATH.replace(PROD_MODEL_PATH)
    cand_meta = CANDIDATE_MODEL_PATH.with_suffix(".json")
    prod_meta = PROD_MODEL_PATH.with_suffix(".json")
    if cand_meta.exists():
        cand_meta.replace(prod_meta)
    log.info("Candidate promoted to production: %s", PROD_MODEL_PATH)


def _publish_ops_event(event_type: str, details: dict) -> None:
    """Append a structured ops event to logs/ops_events.jsonl."""
    ops_log = LOG_DIR / "ops_events.jsonl"
    event = {"event": event_type, "timestamp_ns": time.time_ns(), **details}
    with open(ops_log, "a") as f:
        f.write(json.dumps(event) + "\n")
    log.info("[OPS_EVENT] %s: %s", event_type, details)


def _write_train_log(date_str: str, metrics: dict) -> None:
    out = LOG_DIR / f"train_{date_str}.json"
    with open(out, "w") as f:
        json.dump(metrics, f, indent=2)
    log.info("Train log written: %s", out)

    # Prometheus text format for node_exporter textfile collector
    prom_out = LOG_DIR / "neural_alpha_train.prom"
    lines = [
        "# HELP neural_alpha_train_ic IC of latest trained model",
        "# TYPE neural_alpha_train_ic gauge",
        f"neural_alpha_train_ic {metrics.get('ic_mean', 0.0):.6f}",
        "# HELP neural_alpha_train_icir ICIR of latest trained model",
        "# TYPE neural_alpha_train_icir gauge",
        f"neural_alpha_train_icir {metrics.get('icir', 0.0):.6f}",
        "# HELP neural_alpha_train_sharpe Backtest Sharpe of latest trained model",
        "# TYPE neural_alpha_train_sharpe gauge",
        f"neural_alpha_train_sharpe {metrics.get('sharpe', 0.0):.6f}",
        "# HELP neural_alpha_promoted Whether the candidate was promoted (1=yes)",
        "# TYPE neural_alpha_promoted gauge",
        f"neural_alpha_promoted {1 if metrics.get('promoted') else 0}",
        "# HELP neural_alpha_ensemble_ic IC of ensemble (primary+secondary) model",
        "# TYPE neural_alpha_ensemble_ic gauge",
        f"neural_alpha_ensemble_ic {metrics.get('ensemble_ic_mean', 0.0):.6f}",
        "# HELP neural_alpha_ensemble_icir ICIR of ensemble model",
        "# TYPE neural_alpha_ensemble_icir gauge",
        f"neural_alpha_ensemble_icir {metrics.get('ensemble_icir', 0.0):.6f}",
        "# HELP neural_alpha_ensemble_promoted Whether the ensemble was promoted (1=yes)",
        "# TYPE neural_alpha_ensemble_promoted gauge",
        f"neural_alpha_ensemble_promoted {1 if metrics.get('ensemble_promoted') else 0}",
    ]
    prom_out.write_text("\n".join(lines) + "\n")


def run() -> dict:
    _ensure_dirs()
    date_str = datetime.now(timezone.utc).strftime("%Y%m%d")
    log.info(
        "Daily train started — date=%s  symbol=%s  ticks=%d  epochs=%d",
        date_str,
        TRAIN_SYMBOL,
        TRAIN_TICKS,
        EPOCHS,
    )

    # 1. Collect data
    cached = DATA_DIR / f"ticks_{_SYMBOL_TAG}_{date_str}.parquet"
    if cached.exists():
        import polars as pl
        log.info("Loading cached ticks from %s", cached)
        df = pl.read_parquet(cached)
    else:
        df = collect_l5_ticks(TRAIN_TICKS, TRAIN_INTERVAL_MS, exchanges=["SOLANA"], symbol=TRAIN_SYMBOL)
        df.write_parquet(cached)
        log.info("Tick data cached → %s  rows=%d", cached, len(df))

    # 2. Train
    cfg = TrainerConfig(
        epochs=EPOCHS,
        n_folds=4,
        train_frac=0.75,
        seq_len=64,
        d_spatial=64,
        d_temporal=128,
        pretrain=True,
        pretrain_epochs=5,
    )
    log.info("Starting walk-forward training (%d folds)…", cfg.n_folds)
    fold_results = walk_forward_train(df, cfg)

    if not fold_results:
        log.error("Training produced no fold results — dataset too small.")
        return {"error": "no_fold_results", "date": date_str}

    # 3. Alpha regression
    alpha_metrics = analyse_alpha(fold_results, horizon_idx=2)
    log.info("Alpha: IC=%.4f  ICIR=%.4f  HitRate=%.3f",
             alpha_metrics.ic_mean, alpha_metrics.icir, alpha_metrics.hit_rate)

    # 4. Backtest on held-out fold
    bt_cfg = BacktestConfig(entry_threshold_bps=5.0, taker_fee_bps=5.0)
    last_fold = fold_results[-1]
    bt = NeuralAlphaBacktest(bt_cfg)
    signals = last_fold["predictions"][:, 2]  # mid-horizon = index 2 (100t)
    import polars as pl
    fold_size = len(df) // cfg.n_folds
    test_start = len(df) - fold_size
    test_df = df[test_start:]
    T_test = len(test_df)
    tick_signals = np.zeros(T_test, dtype=np.float32)
    for i, sig in enumerate(signals):
        idx = min(i + cfg.seq_len - 1, T_test - 1)
        tick_signals[idx] = sig
    bt_result = bt.run(test_df, tick_signals)
    bt_metrics = bt_result["metrics"]
    sharpe = bt_metrics.get("sharpe_annualised", 0.0) if "error" not in bt_metrics else 0.0
    log.info("Backtest: trades=%s  PnL=%.4f  Sharpe=%.3f",
             bt_metrics.get("total_trades", 0),
             bt_metrics.get("total_net_pnl", 0.0),
             sharpe)

    # 5. Save candidate
    best_fold = min(fold_results, key=lambda f: f["metrics"].get("loss_total", 1e9))
    torch.save(best_fold["model_state"], CANDIDATE_MODEL_PATH)

    # Save normalisation stats for inference
    raw_scalar = compute_scalar_features(df)
    _, scalar_mean, scalar_std = normalise_scalar(raw_scalar)
    np.savez(NORM_STATS_PATH, mean=scalar_mean, std=scalar_std)

    # Train secondary (smaller) model for ensemble
    cfg_small = TrainerConfig(
        epochs=EPOCHS,
        n_folds=4,
        train_frac=0.75,
        seq_len=64,
        d_spatial=32,
        d_temporal=64,
        n_temp_layers=1,
        pretrain=False,
    )
    log.info("Training secondary model (d_spatial=32, d_temporal=64)…")
    fold_results_small = walk_forward_train(df, cfg_small)
    ensemble_ic_mean = 0.0
    ensemble_icir = 0.0
    ensemble_promoted = False
    if fold_results_small:
        best_small = min(fold_results_small, key=lambda f: f["metrics"].get("loss_total", 1e9))
        torch.save(best_small["model_state"], SECONDARY_MODEL_PATH)
        log.info("Secondary model saved → %s", SECONDARY_MODEL_PATH)
        try:
            blended = _blend_fold_results(fold_results, fold_results_small)
            ensemble_alpha = analyse_alpha(blended, horizon_idx=2)
            ensemble_ic_mean = ensemble_alpha.ic_mean
            ensemble_icir = ensemble_alpha.icir
            log.info(
                "Ensemble alpha: IC=%.4f  ICIR=%.4f  HitRate=%.3f",
                ensemble_ic_mean,
                ensemble_icir,
                ensemble_alpha.hit_rate,
            )
            ensemble_promoted = ensemble_ic_mean >= PROMOTE_IC_MIN and ensemble_ic_mean > alpha_metrics.ic_mean
            if ensemble_promoted:
                log.info(
                    "Ensemble IC %.4f beats primary IC %.4f — ensemble is production candidate",
                    ensemble_ic_mean,
                    alpha_metrics.ic_mean,
                )
        except Exception as exc:
            log.warning("Ensemble alpha evaluation failed: %s", exc)

    train_metrics = {
        "date": date_str,
        "n_ticks": len(df),
        "n_folds": len(fold_results),
        "ic_mean": round(alpha_metrics.ic_mean, 6),
        "ic_std": round(alpha_metrics.ic_std, 6),
        "icir": round(alpha_metrics.icir, 6),
        "hit_rate": round(alpha_metrics.hit_rate, 4),
        "ols_alpha": round(alpha_metrics.ols_alpha, 8),
        "ols_t_stat": round(alpha_metrics.ols_t_stat, 4),
        "backtest_trades": bt_metrics.get("total_trades", 0),
        "backtest_pnl": round(bt_metrics.get("total_net_pnl", 0.0), 4),
        "sharpe": round(sharpe, 4),
        "win_rate": round(bt_metrics.get("win_rate", 0.0), 4),
        "ensemble_ic_mean": round(ensemble_ic_mean, 6),
        "ensemble_icir": round(ensemble_icir, 6),
        "ensemble_promoted": ensemble_promoted,
    }

    registry = ChampionChallengerRegistry(REGISTRY_PATH)
    challenger_id = registry.register_challenger(CANDIDATE_MODEL_PATH, train_metrics)

    # 6. Promote if better than current production model
    prod_ic = _load_prod_model_ic()
    promoted = False
    if alpha_metrics.ic_mean >= PROMOTE_IC_MIN and alpha_metrics.ic_mean > prod_ic:
        log.info("Promoting candidate (IC %.4f > prod IC %.4f)", alpha_metrics.ic_mean, prod_ic)
        _promote_candidate()
        registry.promote(challenger_id, reason="candidate_outperformed_champion")
        promoted = True
        _publish_ops_event("model_promoted", {
            "challenger_id": challenger_id,
            "ic_mean": alpha_metrics.ic_mean,
            "prod_ic": prod_ic,
            "ensemble_ic_mean": ensemble_ic_mean,
            "ensemble_icir": ensemble_icir,
            "ensemble_promoted": ensemble_promoted,
            "date": date_str,
        })
    else:
        reason = (
            f"IC {alpha_metrics.ic_mean:.4f} < threshold {PROMOTE_IC_MIN}"
            if alpha_metrics.ic_mean < PROMOTE_IC_MIN
            else f"IC {alpha_metrics.ic_mean:.4f} <= prod IC {prod_ic:.4f}"
        )
        log.info("Candidate not promoted: %s", reason)
        _publish_ops_event("model_rejected", {
            "challenger_id": challenger_id,
            "reason": reason,
            "ic_mean": alpha_metrics.ic_mean,
            "prod_ic": prod_ic,
            "ensemble_ic_mean": ensemble_ic_mean,
            "date": date_str,
        })

    train_metrics["challenger_id"] = challenger_id
    train_metrics["registry_path"] = str(REGISTRY_PATH)
    train_metrics["promoted"] = promoted
    _save_model_meta(CANDIDATE_MODEL_PATH, train_metrics)
    _write_train_log(date_str, train_metrics)

    log.info("Daily train complete — promoted=%s", promoted)
    return train_metrics


if __name__ == "__main__":
    result = run()
    print(json.dumps(result, indent=2))
    sys.exit(0 if "error" not in result else 1)
