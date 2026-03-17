"""
Hourly SLO sanity-check engine.

Reads existing log artefacts and checks seven algorithm-health SLOs:

    1. signals_alive    — neural alpha is generating signals
    2. ic_nonnegative   — rolling IC >= 0 (signal has predictive power)
    3. fill_rate_ok     — execution fill rate >= 40%
    4. reject_rate_ok   — risk-control reject rate <= 25%
    5. drawdown_ok      — max drawdown within kill-switch threshold
    6. model_fresh      — production model updated within 48 h
    7. no_dq_breach     — no data_quality_failed ops events in last 6 h

Outputs three files on every run:
    logs/slo_report_<YYYYMMDD_HH>.json   timestamped snapshot
    logs/slo_latest.json                  latest run (overwritten)
    logs/slo_engine.prom                  Prometheus textfile for node_exporter

Run via systemd timer every hour (see deploy/systemd/slo-engine.timer).

Environment variables:
    LOG_DIR          Where shadow/ops logs live      (default: project root)
    METRICS_OUT_DIR  Where to write output files     (default: logs/)
    MODEL_DIR        Model checkpoints directory     (default: models/)
    SLO_LOOKBACK_H   Hours of recent log to inspect  (default: 2)
"""
from __future__ import annotations

import glob
import json
import logging
import os
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

ROOT = Path(__file__).parent.parent
sys.path.insert(0, str(ROOT))

from research.backtest.shadow_metrics import analyse_decisions, analyse_signals, load_jsonl

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s",
    handlers=[logging.StreamHandler()],
)
log = logging.getLogger("slo_engine")

LOG_DIR = Path(os.getenv("LOG_DIR", ROOT))
METRICS_OUT_DIR = Path(os.getenv("METRICS_OUT_DIR", ROOT / "logs"))
MODEL_DIR = Path(os.getenv("MODEL_DIR", ROOT / "models"))
LOOKBACK_H = float(os.getenv("SLO_LOOKBACK_H", "2"))

# SLO thresholds — mirrors config/live/risk.yaml
_DRAWDOWN_KILL_USD = -250.0    # kill-switch threshold
_FILL_RATE_MIN = 0.40          # minimum acceptable fill rate
_REJECT_RATE_MAX = 0.25        # maximum acceptable reject fraction
_IC_MIN = 0.0                  # IC must be non-negative
_MODEL_MAX_AGE_H = 48.0        # model must have been trained within 48 h
_OPS_DQ_LOOKBACK_H = 6.0      # look back 6 h for data-quality failures


# ── Log loaders ───────────────────────────────────────────────────────────────

def _load_decisions() -> list[dict]:
    rows: list[dict] = []
    for p in sorted(glob.glob(str(LOG_DIR / "shadow_decisions_*.jsonl"))):
        rows.extend(load_jsonl(p))
    rows.extend(load_jsonl(str(LOG_DIR / "shadow_decisions.jsonl")))
    return rows


def _load_signals() -> list[dict]:
    return load_jsonl(str(LOG_DIR / "neural_alpha_shadow.jsonl"))


def _load_ops_events() -> list[dict]:
    return load_jsonl(str(LOG_DIR / "ops_events.jsonl"))


def _filter_recent(rows: list[dict], hours: float) -> list[dict]:
    """Return rows whose timestamp_ns falls within the last *hours* hours.

    Falls back to the full row set when no rows have a recent timestamp (e.g.
    in dev/test environments where logs are synthetic or very old).
    """
    cutoff_ns = (time.time() - hours * 3600) * 1_000_000_000
    recent = [r for r in rows if r.get("timestamp_ns", 0) >= cutoff_ns]
    return recent if recent else rows


# ── SLO checks ────────────────────────────────────────────────────────────────

def _check_signals_alive(sig_rows: list[dict]) -> dict:
    recent = _filter_recent(sig_rows, LOOKBACK_H)
    count = len(recent)
    ok = count > 0
    return {
        "name": "signals_alive",
        "pass": ok,
        "value": count,
        "threshold": "> 0",
        "detail": (
            f"{count} signals in last {LOOKBACK_H:.0f}h"
            if ok
            else f"No signals in last {LOOKBACK_H:.0f}h — model may be silent or feed disconnected"
        ),
    }


def _check_ic_nonnegative(sig: dict) -> dict:
    ic = sig.get("ic", 0.0)
    ok = ic >= _IC_MIN
    return {
        "name": "ic_nonnegative",
        "pass": ok,
        "value": round(ic, 4),
        "threshold": f">= {_IC_MIN}",
        "detail": f"Rolling IC = {ic:.4f}" + ("" if ok else " (negative — signal may have inverted)"),
    }


def _check_fill_rate(dec: dict) -> dict:
    rate = dec.get("fill_rate", 0.0)
    total = dec.get("total_orders", 0)
    # Pass vacuously when there are no orders yet (e.g. early in session)
    ok = total == 0 or rate >= _FILL_RATE_MIN
    return {
        "name": "fill_rate_ok",
        "pass": ok,
        "value": round(rate, 4),
        "threshold": f">= {_FILL_RATE_MIN:.0%}",
        "detail": (
            "No orders recorded yet"
            if total == 0
            else f"Fill rate {rate:.2%} over {total} orders"
        ),
    }


def _check_reject_rate(dec: dict) -> dict:
    rejects = dec.get("total_rejects", 0)
    terminal = dec.get("total_fills", 0) + dec.get("total_cancels", 0) + rejects
    rate = rejects / terminal if terminal > 0 else 0.0
    ok = terminal == 0 or rate <= _REJECT_RATE_MAX
    return {
        "name": "reject_rate_ok",
        "pass": ok,
        "value": round(rate, 4),
        "threshold": f"<= {_REJECT_RATE_MAX:.0%}",
        "detail": (
            "No terminal orders recorded yet"
            if terminal == 0
            else f"Reject rate {rate:.2%} ({rejects}/{terminal} terminal orders)"
            + ("" if ok else " — risk controls may be over-triggering")
        ),
    }


def _check_drawdown(dec: dict) -> dict:
    dd = dec.get("max_drawdown_usd", 0.0)
    ok = dd > _DRAWDOWN_KILL_USD
    return {
        "name": "drawdown_ok",
        "pass": ok,
        "value": round(dd, 4),
        "threshold": f"> ${_DRAWDOWN_KILL_USD:.0f}",
        "detail": f"Max drawdown ${dd:.2f}" + ("" if ok else " — at or past kill-switch threshold"),
    }


def _check_model_fresh() -> dict:
    candidates = sorted(glob.glob(str(MODEL_DIR / "neural_alpha_*_latest.json")))
    if not candidates:
        return {
            "name": "model_fresh",
            "pass": False,
            "value": None,
            "threshold": f"< {_MODEL_MAX_AGE_H:.0f}h old",
            "detail": "No model metadata file found in models/",
        }
    newest = max(candidates, key=lambda p: Path(p).stat().st_mtime)
    age_h = (time.time() - Path(newest).stat().st_mtime) / 3600
    ok = age_h < _MODEL_MAX_AGE_H
    return {
        "name": "model_fresh",
        "pass": ok,
        "value": round(age_h, 2),
        "threshold": f"< {_MODEL_MAX_AGE_H:.0f}h old",
        "detail": f"Model last updated {age_h:.1f}h ago ({Path(newest).name})"
        + ("" if ok else " — daily_train.py may have failed"),
    }


def _check_no_dq_breach(ops_rows: list[dict]) -> dict:
    recent_ops = _filter_recent(ops_rows, _OPS_DQ_LOOKBACK_H)
    breaches = [r for r in recent_ops if r.get("event") == "data_quality_failed"]
    ok = len(breaches) == 0
    return {
        "name": "no_dq_breach",
        "pass": ok,
        "value": len(breaches),
        "threshold": "0",
        "detail": (
            f"No DQ failures in last {_OPS_DQ_LOOKBACK_H:.0f}h"
            if ok
            else f"{len(breaches)} data_quality_failed event(s) in last {_OPS_DQ_LOOKBACK_H:.0f}h"
        ),
    }


# ── Report & output ───────────────────────────────────────────────────────────

def _print_report(result: dict) -> None:
    ts = result["timestamp_utc"]
    checks = result["checks"]
    summary = result["summary"]

    print()
    print("=" * 64)
    print(f"  SLO Engine — {ts}")
    print("=" * 64)
    for c in checks:
        status = "PASS" if c["pass"] else "FAIL"
        mark = "+" if c["pass"] else "!"
        print(f"  [{mark}] {status:4s}  {c['name']:25s}  {c['detail']}")
    print()
    overall = "ALL PASS" if summary["all_pass"] else f"{summary['failed']}/{summary['total']} FAILING"
    print(f"  Overall: {overall}")
    print("=" * 64)
    print()


def _write_prom(checks: list[dict], prom_path: Path) -> None:
    lines = [
        "# HELP slo_check_pass SLO check result (1=pass 0=fail)",
        "# TYPE slo_check_pass gauge",
    ]
    for c in checks:
        val = 1 if c["pass"] else 0
        lines.append(f'slo_check_pass{{name="{c["name"]}"}} {val}')
    lines += [
        "# HELP slo_all_pass All SLO checks passing (1=yes 0=no)",
        "# TYPE slo_all_pass gauge",
        f"slo_all_pass {1 if all(c['pass'] for c in checks) else 0}",
    ]
    prom_path.write_text("\n".join(lines) + "\n")


# ── Entry point ───────────────────────────────────────────────────────────────

def run() -> dict:
    METRICS_OUT_DIR.mkdir(parents=True, exist_ok=True)

    ts = datetime.now(timezone.utc)
    ts_slot = ts.strftime("%Y%m%d_%H")
    ts_iso = ts.isoformat()
    log.info("SLO engine run — %s  lookback=%.0fh", ts_iso, LOOKBACK_H)

    dec_rows = _load_decisions()
    sig_rows = _load_signals()
    ops_rows = _load_ops_events()

    dec = analyse_decisions(dec_rows)
    sig = analyse_signals(sig_rows)

    checks = [
        _check_signals_alive(sig_rows),
        _check_ic_nonnegative(sig),
        _check_fill_rate(dec),
        _check_reject_rate(dec),
        _check_drawdown(dec),
        _check_model_fresh(),
        _check_no_dq_breach(ops_rows),
    ]

    passed = sum(1 for c in checks if c["pass"])
    failed = len(checks) - passed

    result = {
        "timestamp_utc": ts_iso,
        "lookback_hours": LOOKBACK_H,
        "summary": {
            "total": len(checks),
            "passed": passed,
            "failed": failed,
            "all_pass": failed == 0,
        },
        "checks": checks,
    }

    _print_report(result)

    snap_path = METRICS_OUT_DIR / f"slo_report_{ts_slot}.json"
    with open(snap_path, "w") as f:
        json.dump(result, f, indent=2)

    latest_path = METRICS_OUT_DIR / "slo_latest.json"
    with open(latest_path, "w") as f:
        json.dump(result, f, indent=2)

    _write_prom(checks, METRICS_OUT_DIR / "slo_engine.prom")

    log.info(
        "SLO report written — pass=%d/%d  all_pass=%s  snap=%s",
        passed, len(checks), result["summary"]["all_pass"], snap_path,
    )
    return result


if __name__ == "__main__":
    result = run()
    sys.exit(0 if result["summary"]["all_pass"] else 1)
