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
    DQ_MAX_NULL_RATE Max null fraction per column before gate fails (default: 0.05)
    DQ_MAX_GAP_S     Max inter-tick gap per venue in seconds (default: 300)
    DQ_MAX_AGE_H     Max age of oldest tick in hours (default: 26)
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
from research.neural_alpha.data_quality import (
    DataQualityConfig,
    DataQualityError,
    assert_quality_passed,
    run_quality_gates,
    write_quality_report,
)
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

# Data quality gate thresholds
DQ_MAX_NULL_RATE = float(os.getenv("DQ_MAX_NULL_RATE", "0.05"))
DQ_MAX_GAP_S = float(os.getenv("DQ_MAX_GAP_S", "300"))
DQ_MAX_AGE_H = float(os.getenv("DQ_MAX_AGE_H", "26"))
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
    oos_ic = meta.get("challenger_holdout_ic")
    if oos_ic is not None:
        return float(oos_ic)
    return float(meta.get("ic_mean", -1.0))


def _compute_holdout_ic(model_path: Path, holdout_df, cfg: "TrainerConfig") -> float:
    """
    Run `model_path` on `holdout_df` and return the IC (Spearman correlation
    of the mid-horizon return prediction against realised mid-horizon return).

    Evaluating BOTH champion and challenger on the **same** held-out slice
    eliminates the stale-IC bias that arises when the champion's stored IC was
    computed on a different day's data distribution.

    Returns -1.0 if the model file is missing or the holdout is too small.
    """
    if not model_path.exists():
        return -1.0

    from research.neural_alpha.dataset import DatasetConfig, LOBDataset
    from research.neural_alpha.model import CryptoAlphaNet
    from torch.utils.data import DataLoader

    ds_cfg = DatasetConfig(seq_len=cfg.seq_len)
    dataset = LOBDataset(holdout_df, ds_cfg)
    if len(dataset) < 10:
        return -1.0

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    model = CryptoAlphaNet(
        d_spatial=cfg.d_spatial,
        d_temporal=cfg.d_temporal,
        seq_len=cfg.seq_len,
    ).to(device)
    try:
        state = torch.load(model_path, map_location=device, weights_only=True)
        model.load_state_dict(state, strict=True)
    except Exception as exc:
        log.warning("Could not load model %s for holdout IC evaluation: %s", model_path, exc)
        return -1.0
    model.eval()

    loader = DataLoader(dataset, batch_size=64, shuffle=False)
    preds_list: list[np.ndarray] = []
    labels_list: list[np.ndarray] = []
    with torch.no_grad():
        for batch in loader:
            lob = batch["lob"].to(device)
            scalar = batch["scalar"].to(device)
            out = model(lob, scalar)
            # Mid-horizon return: index 2 of the 4-horizon head (100-tick)
            preds_list.append(out["returns"][:, -1, 2].cpu().numpy())
            labels_list.append(batch["labels"][:, -1, 2].numpy())

    if not preds_list:
        return -1.0
    preds = np.concatenate(preds_list)
    labels = np.concatenate(labels_list)
    if preds.std() < 1e-9 or labels.std() < 1e-9:
        return -1.0
    from scipy.stats import spearmanr
    ic, _ = spearmanr(preds, labels)
    return float(ic) if np.isfinite(ic) else -1.0


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


def _quality_report_path(date_str: str) -> Path:
    return LOG_DIR / f"data_quality_{date_str}.json"


def _run_data_quality_gates(df, date_str: str) -> None:
    """Run data quality gates and write the report. Raises DataQualityError on breach."""
    cfg = DataQualityConfig(
        max_null_rate=DQ_MAX_NULL_RATE,
        max_gap_ns=int(DQ_MAX_GAP_S * 1_000_000_000),
        max_age_ns=int(DQ_MAX_AGE_H * 3_600 * 1_000_000_000),
    )
    report = run_quality_gates(df, cfg)
    write_quality_report(report, _quality_report_path(date_str))
    assert_quality_passed(report)


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
        "# HELP neural_alpha_challenger_holdout_ic Challenger IC on shared OOS holdout",
        "# TYPE neural_alpha_challenger_holdout_ic gauge",
        f"neural_alpha_challenger_holdout_ic {metrics.get('challenger_holdout_ic', 0.0):.6f}",
        "# HELP neural_alpha_champion_holdout_ic Champion IC on the same shared OOS holdout",
        "# TYPE neural_alpha_champion_holdout_ic gauge",
        f"neural_alpha_champion_holdout_ic {metrics.get('champion_holdout_ic', 0.0):.6f}",
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
        df = collect_l5_ticks(TRAIN_TICKS, TRAIN_INTERVAL_MS, exchanges=["BINANCE", "KRAKEN", "OKX", "COINBASE"], symbol=TRAIN_SYMBOL)
        df.write_parquet(cached)
        log.info("Tick data cached → %s  rows=%d", cached, len(df))

    # 2. Data quality gates — fail closed before training or promotion
    try:
        _run_data_quality_gates(df, date_str)
    except DataQualityError as dqe:
        log.error("Data quality gates FAILED — aborting training: %s", dqe)
        _publish_ops_event("data_quality_failed", {
            "date": date_str,
            "breaches": [c["check"] for c in dqe.report.to_dict()["checks"] if not c["passed"]],
            "quality_report_path": str(_quality_report_path(date_str)),
        })
        return {
            "error": "data_quality_failed",
            "date": date_str,
            "quality_report_path": str(_quality_report_path(date_str)),
        }
    log.info("Data quality gates passed.")

    # 3. Train
    cfg = TrainerConfig(
        epochs=EPOCHS,
        n_folds=4,
        train_frac=0.75,
        seq_len=64,
        d_spatial=64,
        d_temporal=128,
        pretrain=True,
        pretrain_epochs=5,
        lr_warmup_epochs=3,
        early_stop_patience=5,
        log_every_epochs=1,
    )
    log.info("Starting walk-forward training (%d folds)…", cfg.n_folds)
    fold_results = walk_forward_train(df, cfg)

    if not fold_results:
        log.error("Training produced no fold results — dataset too small.")
        return {"error": "no_fold_results", "date": date_str}

    # 4. Alpha regression
    alpha_metrics = analyse_alpha(fold_results, horizon_idx=2)
    log.info("Alpha: IC=%.4f  ICIR=%.4f  HitRate=%.3f",
             alpha_metrics.ic_mean, alpha_metrics.icir, alpha_metrics.hit_rate)

    # 5. Backtest on held-out fold
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

    # 6. Save candidate
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
        "quality_report_path": str(_quality_report_path(date_str)),
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

    if registry.current_champion() is None and PROD_MODEL_PATH.exists():
        baseline_ic = champion_holdout_ic if champion_holdout_ic != -1.0 else _load_prod_model_ic()
        bootstrap_meta = {"ic_mean": baseline_ic, "challenger_holdout_ic": baseline_ic, "bootstrapped": True}
        bootstrap_id = registry.register_challenger(PROD_MODEL_PATH, bootstrap_meta)
        registry.promote(bootstrap_id, reason="bootstrap_existing_model")
        log.info(
            "Bootstrapped existing model as champion in registry (holdout IC=%.4f): %s",
            baseline_ic, PROD_MODEL_PATH,
        )
        _publish_ops_event("model_bootstrapped", {
            "model_path": str(PROD_MODEL_PATH),
            "holdout_ic": baseline_ic,
            "date": date_str,
        })

    challenger_id = registry.register_challenger(CANDIDATE_MODEL_PATH, train_metrics)

    # 7. Promote if better than current production model — compared on the SAME
    #    held-out slice so champion and challenger are always benchmarked fairly.
    #    The last fold's test data is used as the shared OOS holdout: it has not
    #    been seen by the challenger during walk-forward training and it uses the
    #    same data distribution as today's session.
    fold_size = len(df) // cfg.n_folds
    holdout_start = len(df) - fold_size
    holdout_df = df[holdout_start:]

    challenger_holdout_ic = _compute_holdout_ic(CANDIDATE_MODEL_PATH, holdout_df, cfg)
    champion_holdout_ic = _compute_holdout_ic(PROD_MODEL_PATH, holdout_df, cfg)
    log.info(
        "Holdout IC — challenger=%.4f  champion=%.4f  (same %d-tick slice)",
        challenger_holdout_ic, champion_holdout_ic, len(holdout_df),
    )

    # Fall back to the stored cross-fold IC when the holdout is too thin or the
    # production model file is missing (e.g. very first run).
    if champion_holdout_ic == -1.0:
        champion_holdout_ic = _load_prod_model_ic()
        log.info("Production model not evaluable on holdout; using stored IC %.4f", champion_holdout_ic)

    promoted = challenger_holdout_ic >= PROMOTE_IC_MIN and challenger_holdout_ic > champion_holdout_ic

    train_metrics["challenger_id"] = challenger_id
    train_metrics["registry_path"] = str(REGISTRY_PATH)
    train_metrics["promoted"] = promoted
    train_metrics["challenger_holdout_ic"] = round(challenger_holdout_ic, 6)
    train_metrics["champion_holdout_ic"] = round(champion_holdout_ic, 6)
    # Save candidate meta BEFORE _promote_candidate() so the .json file exists
    # when promote moves it to latest.json (required for _load_prod_model_ic() on
    # subsequent runs to return the correct IC rather than always -1.0).
    _save_model_meta(CANDIDATE_MODEL_PATH, train_metrics)

    if promoted:
        log.info(
            "Promoting candidate (holdout IC %.4f > champion holdout IC %.4f)",
            challenger_holdout_ic, champion_holdout_ic,
        )
        _promote_candidate()
        registry.promote(challenger_id, reason="candidate_outperformed_champion")
        _publish_ops_event("model_promoted", {
            "challenger_id": challenger_id,
            "ic_mean": alpha_metrics.ic_mean,
            "challenger_holdout_ic": challenger_holdout_ic,
            "champion_holdout_ic": champion_holdout_ic,
            "ensemble_ic_mean": ensemble_ic_mean,
            "ensemble_icir": ensemble_icir,
            "ensemble_promoted": ensemble_promoted,
            "date": date_str,
        })
    else:
        reason = (
            f"holdout IC {challenger_holdout_ic:.4f} < threshold {PROMOTE_IC_MIN}"
            if challenger_holdout_ic < PROMOTE_IC_MIN
            else f"holdout IC {challenger_holdout_ic:.4f} <= champion holdout IC {champion_holdout_ic:.4f}"
        )
        log.info("Candidate not promoted: %s", reason)
        _publish_ops_event("model_rejected", {
            "challenger_id": challenger_id,
            "reason": reason,
            "ic_mean": alpha_metrics.ic_mean,
            "challenger_holdout_ic": challenger_holdout_ic,
            "champion_holdout_ic": champion_holdout_ic,
            "ensemble_ic_mean": ensemble_ic_mean,
            "date": date_str,
        })

    _write_train_log(date_str, train_metrics)

    log.info("Daily train complete — promoted=%s", promoted)
    return train_metrics


if __name__ == "__main__":
    result = run()
    print(json.dumps(result, indent=2))
    sys.exit(0 if "error" not in result else 1)
