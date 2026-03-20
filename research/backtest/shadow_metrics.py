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

_IC_WINDOW = 20
_MAX_EXCHANGE_JUMP_NS = 60 * 1000000000


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


def analyse_decisions(rows: list[dict]) -> dict:
    fills = [r for r in rows if r.get("event") == "FILL"]
    cancels = [r for r in rows if r.get("event") == "CANCELED"]
    rejects = [r for r in rows if r.get("event") == "REJECTED"]
    resting = [r for r in rows if r.get("event") == "RESTING"]
    terminal_orders = len(fills) + len(cancels) + len(rejects)
    fill_rate = len(fills) / terminal_orders if terminal_orders else 0.0
    total_orders = terminal_orders + len(resting)
    pnl_series: list[float] = []
    for r in fills:
        pnl_series.append(r.get("cumulative_pnl", 0.0))
    net_pnl = pnl_series[-1] if pnl_series else 0.0
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
    fill_qty = sum((r.get("qty", 0.0) for r in fills))
    max_dd = 0.0
    peak = 0.0
    for pnl in pnl_series:
        if pnl > peak:
            peak = pnl
        dd = pnl - peak
        if dd < max_dd:
            max_dd = dd
    return {
        "total_orders": total_orders,
        "total_fills": len(fills),
        "total_cancels": len(cancels),
        "total_rejects": len(rejects),
        "total_resting": len(resting),
        "fill_rate": fill_rate,
        "net_pnl_usd": net_pnl,
        "max_drawdown_usd": max_dd,
        "total_volume": fill_qty,
        "exchanges": exchanges,
    }


def analyse_signals(rows: list[dict]) -> dict:
    if not rows:
        return {}
    ordered = sorted(
        rows, key=lambda r: (int(r.get("event_index", 0)), int(r.get("session_elapsed_ns", 0)))
    )
    sigs = np.nan_to_num(np.array([r.get("signal", 0.0) for r in ordered], dtype=np.float64))
    risk = np.nan_to_num(np.array([r.get("risk_score", 0.0) for r in ordered], dtype=np.float64))
    mids = np.nan_to_num(np.array([r.get("mid_price", 0.0) for r in ordered], dtype=np.float64))
    elapsed = np.array([r.get("session_elapsed_ns", 0) for r in ordered], dtype=np.int64)
    signals_for_ic: list[float] = []
    realised_values: list[float] = []
    for i in range(len(ordered) - 1):
        if mids[i] <= 0.0 or mids[i + 1] <= 0.0:
            continue
        signals_for_ic.append(float(ordered[i].get("signal", 0.0)))
        realised_values.append((mids[i + 1] - mids[i]) / mids[i])
    realised = np.asarray(realised_values, dtype=np.float64)
    aligned_sigs = np.asarray(signals_for_ic, dtype=np.float64)
    ic = 0.0
    icir = 0.0
    if len(realised) >= _IC_WINDOW and aligned_sigs.std() > 0:
        ic_series = np.array(
            [
                (
                    float(
                        np.corrcoef(
                            aligned_sigs[i - _IC_WINDOW : i], realised[i - _IC_WINDOW : i]
                        )[0, 1]
                    )
                    if i >= _IC_WINDOW
                    else 0.0
                )
                for i in range(1, len(aligned_sigs) + 1)
            ]
        )
        ic_series = np.nan_to_num(ic_series)
        ic = float(np.mean(ic_series))
        ic_std = float(np.std(ic_series))
        icir = float(ic / ic_std) * math.sqrt(252) if ic_std > 1e-06 else 0.0
    timestamp_quality = analyse_timestamp_quality(ordered)
    duration_min = (
        (int(elapsed[-1]) - int(elapsed[0])) / 1000000000.0 / 60.0 if len(elapsed) > 1 else 0.0
    )
    if timestamp_quality["has_timestamp_issues"]:
        ic = 0.0
        icir = 0.0
    return {
        "total_signals": len(ordered),
        "duration_min": round(duration_min, 2),
        "signal_mean_bps": round(float(np.mean(sigs)) * 10000.0, 4),
        "signal_std_bps": round(float(np.std(sigs)) * 10000.0, 4),
        "avg_risk_score": round(float(np.mean(risk)), 4),
        "ic": round(ic, 4),
        "icir_annualised": round(icir, 4),
        "timestamp_quality": timestamp_quality,
    }


def analyse_timestamp_quality(rows: list[dict]) -> dict:
    diagnostics = {
        "exchange_missing": 0,
        "local_missing": 0,
        "exchange_non_monotonic": 0,
        "local_non_monotonic": 0,
        "event_index_non_monotonic": 0,
        "cross_venue_timestamp_jumps": 0,
        "max_exchange_jump_ns": 0,
        "max_local_gap_ns": 0,
    }
    prev_exchange = None
    prev_local = None
    prev_event_index = None
    prev_by_exchange: dict[str, int] = {}
    for row in rows:
        exchange_ts = int(row.get("timestamp_exchange_ns", row.get("timestamp_ns", 0)))
        local_ts = int(row.get("timestamp_local_ns", row.get("timestamp_ns", 0)))
        event_index = int(row.get("event_index", 0))
        exchange = str(row.get("exchange", "UNKNOWN"))
        if exchange_ts <= 0:
            diagnostics["exchange_missing"] += 1
        if local_ts <= 0:
            diagnostics["local_missing"] += 1
        if prev_exchange is not None and exchange_ts > 0:
            diagnostics["max_exchange_jump_ns"] = max(
                diagnostics["max_exchange_jump_ns"], abs(exchange_ts - prev_exchange)
            )
            if exchange_ts < prev_exchange:
                diagnostics["exchange_non_monotonic"] += 1
        if prev_local is not None and local_ts > 0:
            diagnostics["max_local_gap_ns"] = max(
                diagnostics["max_local_gap_ns"], abs(local_ts - prev_local)
            )
            if local_ts < prev_local:
                diagnostics["local_non_monotonic"] += 1
        if prev_event_index is not None and event_index <= prev_event_index:
            diagnostics["event_index_non_monotonic"] += 1
        prev_exchange_ts = prev_by_exchange.get(exchange)
        if prev_exchange_ts is not None and exchange_ts > 0:
            jump = abs(exchange_ts - prev_exchange_ts)
            diagnostics["max_exchange_jump_ns"] = max(diagnostics["max_exchange_jump_ns"], jump)
            if jump > _MAX_EXCHANGE_JUMP_NS:
                diagnostics["cross_venue_timestamp_jumps"] += 1
        if exchange_ts > 0:
            prev_exchange = exchange_ts
            prev_by_exchange[exchange] = exchange_ts
        if local_ts > 0:
            prev_local = local_ts
        prev_event_index = event_index
    diagnostics["has_timestamp_issues"] = any(value > 0 for value in diagnostics.values())
    return diagnostics


def fill_rate_check(
    shadow_fill_rate: float, backtest_fill_rate: float, tolerance: float = 0.1
) -> dict:
    diff = abs(shadow_fill_rate - backtest_fill_rate)
    ok = diff <= tolerance
    return {
        "shadow_fill_rate": round(shadow_fill_rate, 4),
        "backtest_fill_rate": round(backtest_fill_rate, 4),
        "difference": round(diff, 4),
        "within_tolerance": ok,
        "tolerance": tolerance,
        "verdict": "PASS" if ok else "FAIL",
    }


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
        f"  Resting (open)         : {dec.get('total_resting', 0)}",
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
        quality = sig.get("timestamp_quality", {})
        lines += [
            "── Timestamp quality ───────────────────────────────────────",
            f"  Exchange ts missing    : {quality.get('exchange_missing', 0)}",
            f"  Local ts missing       : {quality.get('local_missing', 0)}",
            f"  Exchange non-monotonic : {quality.get('exchange_non_monotonic', 0)}",
            f"  Local non-monotonic    : {quality.get('local_non_monotonic', 0)}",
            f"  Event-index regressions: {quality.get('event_index_non_monotonic', 0)}",
            f"  Venue timestamp jumps  : {quality.get('cross_venue_timestamp_jumps', 0)}",
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


def main() -> None:
    ap = argparse.ArgumentParser(description="Shadow session metrics rollup")
    ap.add_argument(
        "--decisions",
        type=str,
        default="shadow_decisions.jsonl",
        help="C++ shadow engine decision log",
    )
    ap.add_argument(
        "--signals", type=str, default="neural_alpha_shadow.jsonl", help="Neural alpha signal log"
    )
    ap.add_argument("--out", type=str, default=None, help="Save text report to file")
    ap.add_argument("--prom", type=str, default=None, help="Save Prometheus metrics to file")
    ap.add_argument(
        "--backtest-fill-rate",
        type=float,
        default=None,
        dest="backtest_fill_rate",
        help="Expected fill rate from backtest (for readiness check)",
    )
    args = ap.parse_args()
    dec_rows = load_jsonl(args.decisions)
    sig_rows = load_jsonl(args.signals)
    dec = analyse_decisions(dec_rows)
    sig = analyse_signals(sig_rows)
    print_report(dec, sig, out_path=args.out)
    if args.backtest_fill_rate is not None:
        check = fill_rate_check(dec.get("fill_rate", 0.0), args.backtest_fill_rate)
        print(
            f"Fill-rate readiness: shadow={check['shadow_fill_rate']:.2%}  backtest={check['backtest_fill_rate']:.2%}  verdict={check['verdict']}"
        )
    if args.prom:
        write_prometheus(dec, sig, args.prom)


if __name__ == "__main__":
    main()
