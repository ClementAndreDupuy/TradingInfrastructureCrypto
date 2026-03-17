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

Exit codes:
    0 — all checks passed (or no backtest fill rate provided)
    1 — fill-rate readiness check FAILED
"""
from __future__ import annotations

import argparse
import json
import math
import sys
import warnings
from pathlib import Path

import numpy as np


# ── Loaders ───────────────────────────────────────────────────────────────────

def load_jsonl(path: str) -> list[dict]:
    rows: list[dict] = []
    p = Path(path)
    if not p.exists():
        warnings.warn(f"JSONL file not found: {path}", stacklevel=2)
        return rows
    bad_lines = 0
    with open(p) as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if line:
                try:
                    rows.append(json.loads(line))
                except json.JSONDecodeError as exc:
                    bad_lines += 1
                    warnings.warn(
                        f"{path}:{lineno}: skipping malformed JSON — {exc}",
                        stacklevel=2,
                    )
    if bad_lines:
        warnings.warn(
            f"{path}: {bad_lines} line(s) skipped due to JSON parse errors",
            stacklevel=2,
        )
    return rows


# ── Decision log analysis ─────────────────────────────────────────────────────

def analyse_decisions(rows: list[dict]) -> dict:
    fills    = [r for r in rows if r.get("event") == "FILL"]
    cancels  = [r for r in rows if r.get("event") == "CANCELED"]
    rejects  = [r for r in rows if r.get("event") == "REJECTED"]
    resting  = [r for r in rows if r.get("event") == "RESTING"]

    total_orders = len(fills) + len(cancels) + len(rejects) + len(resting)
    fill_rate = len(fills) / total_orders if total_orders else 0.0

    # Sort fills by timestamp so pnl_series is chronologically ordered.
    # cumulative_pnl is only meaningful as the *last* value in time order.
    fills_sorted = sorted(fills, key=lambda r: r.get("timestamp_ns", 0))
    pnl_series: list[float] = [r.get("cumulative_pnl", 0.0) for r in fills_sorted]

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

    fill_qty = sum(r.get("qty", 0.0) for r in fills)

    # Max drawdown from time-ordered cumulative P&L series
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

    # IC: signal vs realised return (next-tick mid return).
    # Only compute where mid_price is valid (> 0); mask bad ticks to avoid
    # spurious IC from zero-price sentinel values.
    mids = np.array([r.get("mid_price", 0.0) for r in rows], dtype=np.float64)
    valid_mid = mids > 0
    with np.errstate(invalid="ignore", divide="ignore"):
        realised = np.where(
            valid_mid[:-1] & valid_mid[1:],
            (mids[1:] - mids[:-1]) / mids[:-1],
            np.nan,
        )

    ic = 0.0
    icir = 0.0
    # Need at least 10 non-NaN pairs and non-constant signals to compute IC.
    valid_pairs = ~np.isnan(realised)
    if valid_pairs.sum() >= 10 and np.nanstd(sigs[:-1][valid_pairs]) > 0:
        N = len(realised)
        WIN = 20
        ic_list: list[float] = []
        for i in range(1, N + 1):
            start = max(0, i - WIN)
            s_win = sigs[start:i]
            r_win = realised[start:i]
            # Require at least 2 finite pairs in the window.
            mask = ~np.isnan(r_win)
            if mask.sum() < 2:
                ic_list.append(0.0)
                continue
            s_w, r_w = s_win[mask], r_win[mask]
            if np.std(s_w) < 1e-15 or np.std(r_w) < 1e-15:
                ic_list.append(0.0)
                continue
            ic_list.append(float(np.corrcoef(s_w, r_w)[0, 1]))

        ic_series = np.nan_to_num(np.array(ic_list, dtype=np.float64))
        ic   = float(np.mean(ic_series))
        # Use ddof=1 for unbiased std; guard against near-zero std.
        ic_std = float(np.std(ic_series, ddof=1))
        icir = float(ic / (ic_std + 1e-9)) * math.sqrt(252)

    # Duration: filter out zero/missing timestamps before computing span.
    valid_ts = ts[ts > 0]
    duration_min = (
        (int(valid_ts[-1]) - int(valid_ts[0])) / 1e9 / 60.0
        if len(valid_ts) > 1
        else 0.0
    )

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

def print_report(
    dec: dict,
    sig: dict,
    out_path: str | None = None,
    fill_check: dict | None = None,
) -> None:
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

    if fill_check:
        verdict = fill_check.get("verdict", "N/A")
        lines += [
            "── Fill-rate readiness check ────────────────────────────────",
            f"  Shadow fill rate       : {fill_check.get('shadow_fill_rate', 0.0):.2%}",
            f"  Backtest fill rate     : {fill_check.get('backtest_fill_rate', 0.0):.2%}",
            f"  Difference             : {fill_check.get('difference', 0.0):.2%}",
            f"  Tolerance              : {fill_check.get('tolerance', 0.0):.2%}",
            f"  Verdict                : {verdict}",
            "",
        ]

    lines += [
        "── Readiness checklist ──────────────────────────────────────",
        "  [ ] Run >= 2 weeks before promoting to live.",
        "  [ ] Shadow fill rates must match backtest within 10%.",
        "  [ ] Kill switch drill must be completed.",
        "  [ ] All monitoring dashboards must be operational.",
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
    ap = argparse.ArgumentParser(
        description="Shadow session metrics rollup",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    ap.add_argument("--decisions",          type=str,   default="shadow_decisions.jsonl",
                    help="C++ shadow engine decision log")
    ap.add_argument("--signals",            type=str,   default="neural_alpha_shadow.jsonl",
                    help="Neural alpha signal log")
    ap.add_argument("--out",                type=str,   default=None,
                    help="Save text report to file")
    ap.add_argument("--prom",               type=str,   default=None,
                    help="Save Prometheus metrics to file")
    ap.add_argument("--backtest-fill-rate", type=float, default=None,
                    dest="backtest_fill_rate",
                    help="Expected fill rate from backtest (for readiness check)")
    ap.add_argument("--tolerance",          type=float, default=0.10,
                    help="Absolute fill-rate tolerance for readiness check")
    args = ap.parse_args()

    dec_rows = load_jsonl(args.decisions)
    sig_rows = load_jsonl(args.signals)

    dec = analyse_decisions(dec_rows)
    sig = analyse_signals(sig_rows)

    check: dict | None = None
    if args.backtest_fill_rate is not None:
        check = fill_rate_check(
            dec.get("fill_rate", 0.0),
            args.backtest_fill_rate,
            tolerance=args.tolerance,
        )

    # print_report now includes the fill-rate check block when provided,
    # so the saved report file is complete.
    print_report(dec, sig, out_path=args.out, fill_check=check)

    if args.prom:
        write_prometheus(dec, sig, args.prom)

    # Non-zero exit so CI pipelines detect a failed readiness check.
    if check is not None and check["verdict"] == "FAIL":
        sys.exit(1)


if __name__ == "__main__":
    main()
