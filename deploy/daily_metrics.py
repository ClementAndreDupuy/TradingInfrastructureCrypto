"""
Daily performance metrics report.

Reads yesterday's shadow/live logs and produces:
  - Text report printed to stdout
  - logs/metrics_<date>.json
  - logs/daily_metrics.prom  (Prometheus textfile collector)

Run via cron at 01:00 UTC (after daily_train.py):
    0 1 * * * /path/to/venv/bin/python /path/to/deploy/daily_metrics.py

Sources read (all optional — missing files are skipped):
    shadow_decisions_*.jsonl   C++ shadow engine fills / cancels
    neural_alpha_shadow.jsonl  Neural alpha signal log
    logs/train_<yesterday>.json Previous day's training result

Environment variables:
    LOG_DIR          Where shadow logs live (default: project root)
    METRICS_OUT_DIR  Where to write metrics files (default: logs/)
"""
from __future__ import annotations

import glob
import json
import logging
import os
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

import numpy as np

ROOT = Path(__file__).parent.parent
sys.path.insert(0, str(ROOT))

from research.backtest.shadow_metrics import (
    analyse_decisions,
    analyse_signals,
    load_jsonl,
    write_prometheus,
)

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s",
    handlers=[logging.StreamHandler()],
)
log = logging.getLogger("daily_metrics")

LOG_DIR = Path(os.getenv("LOG_DIR", ROOT))
METRICS_OUT_DIR = Path(os.getenv("METRICS_OUT_DIR", ROOT / "logs"))
METRICS_OUT_DIR.mkdir(parents=True, exist_ok=True)


def _load_shadow_decisions() -> list[dict]:
    pattern = str(LOG_DIR / "shadow_decisions_*.jsonl")
    rows: list[dict] = []
    for path in sorted(glob.glob(pattern)):
        rows.extend(load_jsonl(path))
    if not rows:
        rows = load_jsonl(str(LOG_DIR / "shadow_decisions.jsonl"))
    return rows


def _load_train_meta(date_str: str) -> dict:
    path = METRICS_OUT_DIR / f"train_{date_str}.json"
    if not path.exists():
        yesterday = (datetime.now(timezone.utc) - timedelta(days=1)).strftime("%Y%m%d")
        path = METRICS_OUT_DIR / f"train_{yesterday}.json"
    if path.exists():
        with open(path) as f:
            return json.load(f)
    return {}


def _sharpe_from_pnl_series(pnl_series: list[float]) -> float:
    if len(pnl_series) < 2:
        return 0.0
    daily = np.diff(pnl_series)
    std = np.std(daily, ddof=1)
    if std < 1e-9:
        return 0.0
    return float(np.mean(daily) / std * np.sqrt(252))


def run() -> dict:
    date_str = datetime.now(timezone.utc).strftime("%Y%m%d")
    log.info("Daily metrics — date=%s", date_str)

    dec_rows = _load_shadow_decisions()
    sig_rows = load_jsonl(str(LOG_DIR / "neural_alpha_shadow.jsonl"))
    train_meta = _load_train_meta(date_str)

    dec = analyse_decisions(dec_rows)
    sig = analyse_signals(sig_rows)

    pnl_series = [r.get("cumulative_pnl", 0.0)
                  for r in dec_rows if r.get("event") == "FILL"]
    sharpe = _sharpe_from_pnl_series(pnl_series)

    metrics = {
        "date": date_str,
        "shadow": {
            "total_orders": dec.get("total_orders", 0),
            "total_fills": dec.get("total_fills", 0),
            "fill_rate": round(dec.get("fill_rate", 0.0), 4),
            "net_pnl_usd": round(dec.get("net_pnl_usd", 0.0), 4),
            "max_drawdown_usd": round(dec.get("max_drawdown_usd", 0.0), 4),
            "total_volume_btc": round(dec.get("total_volume", 0.0), 6),
            "sharpe_daily": round(sharpe, 4),
        },
        "signal": {
            "total_signals": sig.get("total_signals", 0),
            "signal_mean_bps": sig.get("signal_mean_bps", 0.0),
            "signal_std_bps": sig.get("signal_std_bps", 0.0),
            "ic": sig.get("ic", 0.0),
            "icir_annualised": sig.get("icir_annualised", 0.0),
            "avg_risk_score": sig.get("avg_risk_score", 0.0),
        },
        "model": {
            "ic_mean": train_meta.get("ic_mean", None),
            "icir": train_meta.get("icir", None),
            "backtest_sharpe": train_meta.get("sharpe", None),
            "promoted": train_meta.get("promoted", None),
            "train_date": train_meta.get("date", None),
        },
        "readiness": _readiness_check(dec, sig, train_meta),
    }

    _print_report(metrics)
    _write_json(date_str, metrics)
    _write_prometheus_metrics(dec, sig, metrics)

    return metrics


def _readiness_check(dec: dict, sig: dict, train_meta: dict) -> dict:
    checks = {}
    fill_rate = dec.get("fill_rate", 0.0)
    checks["fill_rate_ok"] = fill_rate >= 0.5
    checks["ic_positive"] = (train_meta.get("ic_mean") or 0.0) > 0.0
    checks["no_drawdown_breach"] = dec.get("max_drawdown_usd", 0.0) > -500.0
    checks["signals_generating"] = sig.get("total_signals", 0) > 0
    checks["all_pass"] = all(checks.values())
    return checks


def _print_report(m: dict) -> None:
    shadow = m["shadow"]
    signal = m["signal"]
    model = m["model"]
    readiness = m["readiness"]

    print()
    print("=" * 60)
    print(f"  Daily Metrics — {m['date']}")
    print("=" * 60)
    print()
    print("── Shadow execution ────────────────────────────────────────")
    print(f"  Orders / Fills     : {shadow['total_orders']} / {shadow['total_fills']}")
    print(f"  Fill rate          : {shadow['fill_rate']:.2%}")
    print(f"  Net P&L            : ${shadow['net_pnl_usd']:.4f}")
    print(f"  Max drawdown       : ${shadow['max_drawdown_usd']:.4f}")
    print(f"  Volume (BTC)       : {shadow['total_volume_btc']:.6f}")
    print(f"  Sharpe (daily est) : {shadow['sharpe_daily']:.3f}")
    print()
    print("── Neural alpha signal ─────────────────────────────────────")
    print(f"  Signals            : {signal['total_signals']}")
    print(f"  Mean / Std (bps)   : {signal['signal_mean_bps']:.4f} / {signal['signal_std_bps']:.4f}")
    print(f"  IC (rolling 20)    : {signal['ic']:.4f}")
    print(f"  ICIR (annualised)  : {signal['icir_annualised']:.4f}")
    print(f"  Avg risk score     : {signal['avg_risk_score']:.4f}")
    print()
    print("── Model ───────────────────────────────────────────────────")
    print(f"  Train date         : {model['train_date'] or 'n/a'}")
    print(f"  IC / ICIR          : {model['ic_mean'] or 'n/a'} / {model['icir'] or 'n/a'}")
    print(f"  Backtest Sharpe    : {model['backtest_sharpe'] or 'n/a'}")
    print(f"  Promoted           : {model['promoted']}")
    print()
    print("── Readiness ───────────────────────────────────────────────")
    for k, v in readiness.items():
        status = "PASS" if v else "FAIL"
        print(f"  {k:30s}: {status}")
    print()
    print("=" * 60)
    print()


def _write_json(date_str: str, metrics: dict) -> None:
    out = METRICS_OUT_DIR / f"metrics_{date_str}.json"
    with open(out, "w") as f:
        json.dump(metrics, f, indent=2)
    log.info("Metrics written: %s", out)


def _write_prometheus_metrics(dec: dict, sig: dict, full: dict) -> None:
    shadow = full["shadow"]
    readiness = full["readiness"]
    prom_path = str(METRICS_OUT_DIR / "daily_metrics.prom")

    write_prometheus(dec, sig, prom_path)

    # Append extra metrics not in shadow_metrics.py
    extra = [
        "\n# HELP trading_sharpe_daily Daily Sharpe estimate from shadow fills",
        "# TYPE trading_sharpe_daily gauge",
        f"trading_sharpe_daily {shadow['sharpe_daily']:.6f}",
        "# HELP trading_readiness All readiness checks passing (1=yes)",
        "# TYPE trading_readiness gauge",
        f"trading_readiness {1 if readiness['all_pass'] else 0}",
    ]
    with open(prom_path, "a") as f:
        f.write("\n".join(extra) + "\n")

    log.info("Prometheus metrics appended: %s", prom_path)


if __name__ == "__main__":
    result = run()
    sys.exit(0 if "error" not in result else 1)
