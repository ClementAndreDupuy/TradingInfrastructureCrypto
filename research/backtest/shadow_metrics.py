"""
Shadow validation metrics rollup.

Reads two JSONL logs produced during a shadow session:
    1. shadow_decisions.jsonl — from the C++ ShadowEngine (fills, cancels, rejects)
    2. neural_alpha_shadow.jsonl — from neural alpha shadow session (signals)

Produces a text report and optionally a Prometheus metrics file.

Usage:
    python research/backtest/shadow_metrics.py \\
        --decisions shadow_decisions.jsonl \\
        --signals   neural_alpha_shadow.jsonl \\
        --out       shadow_report.txt \\
        --prom      shadow_metrics.prom
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np
import polars as pl


# ── Loaders ───────────────────────────────────────────────────────────────────

def load_jsonl(path: str) -> list[dict]:
    rows: list[dict] = []
    p = Path(path)
    if not p.exists():
        return rows
    with open(p) as f:
        for line in f:
            line = line.strip()
            if line:
                try:
                    rows.append(json.loads(line))
                except json.JSONDecodeError:
                    pass
    return rows


# ── Decision log analysis ─────────────────────────────────────────────────────

def analyse_decisions(rows: list[dict]) -> dict:
    fills    = [r for r in rows if r.get("event") == "FILL"]
    cancels  = [r for r in rows if r.get("event") == "CANCELED"]
    rejects  = [r for r in rows if r.get("event") == "REJECTED"]
    resting  = [r for r in rows if r.get("event") == "RESTING"]

    total_orders = len(fills) + len(cancels) + len(rejects) + len(resting)
    fill_rate = len(fills) / total_orders if total_orders else 0.0

    pnl_series: list[float] = []
    for r in fills:
        pnl_series.append(r.get("cumulative_pnl", 0.0))

    net_pnl = pnl_series[-1] if pnl_series else 0.0

    # Per-exchange breakdown
    exchanges: dict[str, dict] = {}
    for r in fills:
        ex = r.get("exchange", "UNKNOWN")
        if ex not in exchanges:
            exchanges[ex] = {"fills": 0, "maker": 0, "taker": 0, "fees": 0.0}
        exchanges[ex]["fills"] += 1
        if r.get("maker"):
            exchanges[ex]["maker"] += 1
        else:
            exchanges[ex]["taker"] += 1
        exchanges[ex]["fees"] += r.get("fee_usd", 0.0)

    # Latency: time from submit to fill (submit_ts_ns not in fill log, use fill ts only)
    fill_qty = sum(r.get("qty", 0.0) for r in fills)

    # Max drawdown from cumulative P&L series
    max_dd = 0.0
    peak   = 0.0
    for pnl in pnl_series:
        if pnl > peak:
            peak = pnl
        dd = pnl - peak
        if dd < max_dd:
            max_dd = dd

    return {
        "total_orders":  total_orders,
        "total_fills":   len(fills),
        "total_cancels": len(cancels),
        "total_rejects": len(rejects),
        "fill_rate":     fill_rate,
        "net_pnl_usd":   net_pnl,
        "max_drawdown_usd": max_dd,
        "total_volume":  fill_qty,
        "exchanges":     exchanges,
    }


# ── Signal log analysis ───────────────────────────────────────────────────────

def analyse_signals(rows: list[dict]) -> dict:
    if not rows:
        return {}

    sigs = np.array([r.get("signal", 0.0) for r in rows], dtype=np.float64)
    risk = np.array([r.get("risk_score", 0.0) for r in rows], dtype=np.float64)
    ts   = np.array([r.get("timestamp_ns", 0) for r in rows], dtype=np.int64)

    # IC: signal vs realised return (next-tick mid return)
    mids = np.array([r.get("mid_price", 0.0) for r in rows], dtype=np.float64)
    with np.errstate(invalid="ignore", divide="ignore"):
        realised = np.where(mids[:-1] > 0, (mids[1:] - mids[:-1]) / mids[:-1], 0.0)

    ic = 0.0
    icir = 0.0
    if len(realised) >= 10 and sigs[:-1].std() > 0:
        ic_series = np.array([
            float(np.corrcoef(sigs[max(0, i-20):i], realised[max(0, i-20):i])[0, 1])
            if i >= 2 else 0.0
            for i in range(1, len(sigs))
        ])
        ic_series = np.nan_to_num(ic_series)
        ic   = float(np.mean(ic_series))
        icir = float(ic / (np.std(ic_series) + 1e-9)) * math.sqrt(252)

    # Duration in minutes
    duration_min = (int(ts[-1]) - int(ts[0])) / 1e9 / 60.0 if len(ts) > 1 else 0.0

    return {
        "total_signals":   len(rows),
        "duration_min":    round(duration_min, 2),
        "signal_mean_bps": round(float(np.mean(sigs)) * 1e4, 4),
        "signal_std_bps":  round(float(np.std(sigs))  * 1e4, 4),
        "avg_risk_score":  round(float(np.mean(risk)), 4),
        "ic":              round(ic,   4),
        "icir_annualised": round(icir, 4),
    }


# ── Comparison: shadow fill rate vs backtest target ───────────────────────────

def fill_rate_check(shadow_fill_rate: float, backtest_fill_rate: float,
                    tolerance: float = 0.10) -> dict:
    diff = abs(shadow_fill_rate - backtest_fill_rate)
    ok   = diff <= tolerance
    return {
        "shadow_fill_rate":   round(shadow_fill_rate, 4),
        "backtest_fill_rate": round(backtest_fill_rate, 4),
        "difference":         round(diff, 4),
        "within_tolerance":   ok,
        "tolerance":          tolerance,
        "verdict":            "PASS" if ok else "FAIL",
    }


# ── Report ────────────────────────────────────────────────────────────────────

def print_report(dec: dict, sig: dict, out_path: str | None = None) -> None:
    lines = [
        "=" * 60,
        "  Shadow Validation Report",
        "=" * 60,
        "",
        "── Order execution ─────────────────────────────────────────",
        f"  Total orders submitted : {dec.get('total_orders', 0)}",
        f"  Fills                  : {dec.get('total_fills', 0)}",
        f"  Cancels                : {dec.get('total_cancels', 0)}",
        f"  Rejects                : {dec.get('total_rejects', 0)}",
        f"  Fill rate              : {dec.get('fill_rate', 0.0):.2%}",
        f"  Net P&L                : ${dec.get('net_pnl_usd', 0.0):.4f}",
        f"  Max drawdown           : ${dec.get('max_drawdown_usd', 0.0):.4f}",
        f"  Total volume (qty)     : {dec.get('total_volume', 0.0):.6f}",
        "",
    ]

    if dec.get("exchanges"):
        lines.append("── Per-exchange breakdown ───────────────────────────────────")
        for ex, s in dec["exchanges"].items():
            maker_pct = s["maker"] / s["fills"] if s["fills"] else 0.0
            lines.append(
                f"  {ex:10s}  fills={s['fills']}  maker={maker_pct:.0%}  fees=${s['fees']:.4f}"
            )
        lines.append("")

    if sig:
        lines += [
            "── Neural alpha signal ──────────────────────────────────────",
            f"  Signals generated      : {sig.get('total_signals', 0)}",
            f"  Session duration       : {sig.get('duration_min', 0):.1f} min",
            f"  Mean signal            : {sig.get('signal_mean_bps', 0):.4f} bps",
            f"  Signal std             : {sig.get('signal_std_bps', 0):.4f} bps",
            f"  Avg risk score         : {sig.get('avg_risk_score', 0):.4f}",
            f"  IC (rolling 20)        : {sig.get('ic', 0):.4f}",
            f"  ICIR (annualised)      : {sig.get('icir_annualised', 0):.4f}",
            "",
        ]

    lines += [
        "── Readiness check ─────────────────────────────────────────",
        "  Run >= 2 weeks before promoting to live.",
        "  Shadow fill rates must match backtest within 10%.",
        "  Kill switch drill must be completed.",
        "  All monitoring dashboards must be operational.",
        "",
        "=" * 60,
    ]

    report = "\n".join(lines)
    print(report)

    if out_path:
        Path(out_path).write_text(report)
        print(f"Report saved → {out_path}")


# ── Prometheus export ─────────────────────────────────────────────────────────

def write_prometheus(dec: dict, sig: dict, path: str) -> None:
    lines = [
        "# HELP shadow_fill_rate Fraction of shadow orders that filled",
        "# TYPE shadow_fill_rate gauge",
        f"shadow_fill_rate {dec.get('fill_rate', 0.0):.6f}",
        "# HELP shadow_net_pnl_usd Cumulative net P&L in USD",
        "# TYPE shadow_net_pnl_usd gauge",
        f"shadow_net_pnl_usd {dec.get('net_pnl_usd', 0.0):.6f}",
        "# HELP shadow_max_drawdown_usd Maximum drawdown in USD",
        "# TYPE shadow_max_drawdown_usd gauge",
        f"shadow_max_drawdown_usd {dec.get('max_drawdown_usd', 0.0):.6f}",
        "# HELP shadow_total_fills Total number of shadow fills",
        "# TYPE shadow_total_fills counter",
        f"shadow_total_fills {dec.get('total_fills', 0)}",
    ]

    if sig:
        lines += [
            "# HELP neural_alpha_ic Rolling information coefficient",
            "# TYPE neural_alpha_ic gauge",
            f"neural_alpha_ic {sig.get('ic', 0.0):.6f}",
            "# HELP neural_alpha_icir_annualised Annualised ICIR",
            "# TYPE neural_alpha_icir_annualised gauge",
            f"neural_alpha_icir_annualised {sig.get('icir_annualised', 0.0):.6f}",
            "# HELP neural_alpha_signal_mean_bps Mean signal in bps",
            "# TYPE neural_alpha_signal_mean_bps gauge",
            f"neural_alpha_signal_mean_bps {sig.get('signal_mean_bps', 0.0):.6f}",
        ]

    Path(path).write_text("\n".join(lines) + "\n")
    print(f"Prometheus metrics → {path}")


# ── Entry point ───────────────────────────────────────────────────────────────

def main() -> None:
    ap = argparse.ArgumentParser(description="Shadow session metrics rollup")
    ap.add_argument("--decisions",         type=str,   default="shadow_decisions.jsonl",
                    help="C++ shadow engine decision log")
    ap.add_argument("--signals",           type=str,   default="neural_alpha_shadow.jsonl",
                    help="Neural alpha signal log")
    ap.add_argument("--out",               type=str,   default=None,
                    help="Save text report to file")
    ap.add_argument("--prom",              type=str,   default=None,
                    help="Save Prometheus metrics to file")
    ap.add_argument("--backtest-fill-rate", type=float, default=None,
                    dest="backtest_fill_rate",
                    help="Expected fill rate from backtest (for readiness check)")
    args = ap.parse_args()

    dec_rows = load_jsonl(args.decisions)
    sig_rows = load_jsonl(args.signals)

    dec = analyse_decisions(dec_rows)
    sig = analyse_signals(sig_rows)

    print_report(dec, sig, out_path=args.out)

    if args.backtest_fill_rate is not None:
        check = fill_rate_check(dec.get("fill_rate", 0.0), args.backtest_fill_rate)
        print(
            f"Fill-rate readiness: shadow={check['shadow_fill_rate']:.2%}  "
            f"backtest={check['backtest_fill_rate']:.2%}  "
            f"verdict={check['verdict']}"
        )

    if args.prom:
        write_prometheus(dec, sig, args.prom)


if __name__ == "__main__":
    main()
