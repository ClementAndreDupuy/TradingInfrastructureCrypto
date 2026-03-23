"""
Shadow validation metrics rollup.

Reads JSONL logs produced during a shadow session:
    1. shadow_decisions.jsonl — from the C++ ShadowEngine
    2. neural_alpha_shadow.jsonl — from neural alpha shadow session
    3. ops_events.jsonl — structured training and runtime operations events

Produces a text report and optionally a Prometheus metrics file.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Any

import numpy as np

_IC_WINDOW = 20
_MAX_EXCHANGE_JUMP_NS = 60 * 1000000000
_DEFAULT_DECISIONS_LOG = "shadow_decisions.jsonl"
_DEFAULT_SIGNALS_LOG = "neural_alpha_shadow.jsonl"
_DEFAULT_OPS_LOG = "logs/ops_events.jsonl"


def load_jsonl(path: str) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    p = Path(path)
    if not p.exists():
        return rows
    with open(p, encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    return rows


def analyse_decisions(rows: list[dict[str, Any]]) -> dict[str, Any]:
    fills = [r for r in rows if r.get("event") == "FILL"]
    cancels = [r for r in rows if r.get("event") == "CANCELED"]
    rejects = [r for r in rows if r.get("event") == "REJECTED"]
    resting = [r for r in rows if r.get("event") == "RESTING"]
    transitions = [r for r in rows if r.get("event") == "STATE_TRANSITION"]
    terminal_orders = len(fills) + len(cancels) + len(rejects)
    fill_rate = len(fills) / terminal_orders if terminal_orders else 0.0
    total_orders = terminal_orders + len(resting)
    pnl_series = [float(r.get("cumulative_pnl", 0.0)) for r in fills]
    net_pnl = pnl_series[-1] if pnl_series else 0.0
    exchanges: dict[str, dict[str, Any]] = {}
    total_fees = 0.0
    total_shortfall = 0.0
    total_markout = 0.0
    total_edge = 0.0
    spread_paid = 0.0
    spread_captured = 0.0
    weighted_hold_ms = 0.0
    weighted_inventory_age_ms = 0.0
    fill_qty = 0.0
    venue_contribution: dict[str, float] = {}
    open_lots: list[dict[str, float]] = []
    realised_slippage = 0.0
    stale_inventory = 0.0
    for record in fills:
        exchange = str(record.get("exchange", "UNKNOWN"))
        if exchange not in exchanges:
            exchanges[exchange] = {
                "fills": 0,
                "maker": 0,
                "taker": 0,
                "fees": 0.0,
                "shortfall_bps": 0.0,
                "markout_bps": 0.0,
                "edge_bps": 0.0,
                "hold_ms": 0.0,
                "net_pnl_usd": 0.0,
                "qty": 0.0,
            }
            venue_contribution[exchange] = 0.0
        qty = float(record.get("qty", 0.0))
        fee = float(record.get("fee_usd", 0.0))
        fill_px = float(record.get("fill_px", 0.0))
        shortfall_bps = float(record.get("implementation_shortfall_bps", 0.0))
        markout_bps = float(record.get("markout_bps", 0.0))
        edge_bps = float(record.get("edge_at_entry_bps", 0.0))
        hold_ms = float(record.get("hold_time_ms", 0.0))
        side = str(record.get("side", "BID"))
        decision_mid = float(record.get("decision_mid", 0.0))
        exchanges[exchange]["fills"] += 1
        if record.get("maker"):
            exchanges[exchange]["maker"] += 1
        else:
            exchanges[exchange]["taker"] += 1
        exchanges[exchange]["fees"] += fee
        exchanges[exchange]["shortfall_bps"] += shortfall_bps * qty
        exchanges[exchange]["markout_bps"] += markout_bps * qty
        exchanges[exchange]["edge_bps"] += edge_bps * qty
        exchanges[exchange]["hold_ms"] += hold_ms * qty
        exchanges[exchange]["qty"] += qty
        total_fees += fee
        total_shortfall += shortfall_bps * qty
        total_markout += markout_bps * qty
        total_edge += edge_bps * qty
        weighted_hold_ms += hold_ms * qty
        weighted_inventory_age_ms += hold_ms * qty
        fill_qty += qty
        if decision_mid > 0.0 and fill_px > 0.0:
            signed_spread_bps = abs(fill_px - decision_mid) / decision_mid * 10000.0
            if record.get("maker"):
                spread_captured += signed_spread_bps * qty
            else:
                spread_paid += signed_spread_bps * qty
        contribution = -fee - (shortfall_bps + max(0.0, -markout_bps)) * qty * max(fill_px, 0.0) / 10000.0
        venue_contribution[exchange] += contribution
        exchanges[exchange]["net_pnl_usd"] += contribution
        if hold_ms > 1000.0:
            stale_inventory += qty
        realised_slippage += shortfall_bps * qty * max(fill_px, 0.0) / 10000.0
        remaining = qty
        if side == "BID":
            open_lots.append({"qty": qty, "ts_ns": float(record.get("ts_ns", 0.0))})
        else:
            while remaining > 1e-12 and open_lots:
                lot = open_lots[0]
                matched = min(remaining, float(lot["qty"]))
                age_ms = max(0.0, (float(record.get("ts_ns", 0.0)) - float(lot["ts_ns"])) / 1000000.0)
                weighted_inventory_age_ms += age_ms * matched
                lot["qty"] = float(lot["qty"]) - matched
                remaining -= matched
                if float(lot["qty"]) <= 1e-12:
                    open_lots.pop(0)
    max_dd = 0.0
    peak = 0.0
    for pnl in pnl_series:
        if pnl > peak:
            peak = pnl
        dd = pnl - peak
        if dd < max_dd:
            max_dd = dd
    for exchange, stats in exchanges.items():
        qty = stats["qty"]
        if qty > 0.0:
            stats["shortfall_bps"] /= qty
            stats["markout_bps"] /= qty
            stats["edge_bps"] /= qty
            stats["hold_ms"] /= qty
    avg_inventory_age_ms = weighted_inventory_age_ms / fill_qty if fill_qty else 0.0
    avg_hold_ms = weighted_hold_ms / fill_qty if fill_qty else 0.0
    fee_drag = -total_fees
    slippage_cost = -(realised_slippage / fill_qty) if fill_qty else 0.0
    adverse_selection_cost = -(total_markout / fill_qty) if fill_qty else 0.0
    spread_paid_bps = spread_paid / fill_qty if fill_qty else 0.0
    spread_captured_bps = spread_captured / fill_qty if fill_qty else 0.0
    venue_worst = min(venue_contribution.items(), key=lambda item: item[1])[0] if venue_contribution else "UNKNOWN"
    return {
        "total_orders": total_orders,
        "total_fills": len(fills),
        "total_cancels": len(cancels),
        "total_rejects": len(rejects),
        "total_resting": len(resting),
        "state_transition_count": len(transitions),
        "state_transitions": transitions,
        "fill_rate": fill_rate,
        "net_pnl_usd": net_pnl,
        "max_drawdown_usd": max_dd,
        "total_volume": fill_qty,
        "exchanges": exchanges,
        "fees_usd": total_fees,
        "fee_drag_usd": fee_drag,
        "implementation_shortfall_bps": total_shortfall / fill_qty if fill_qty else 0.0,
        "markout_bps": total_markout / fill_qty if fill_qty else 0.0,
        "edge_at_entry_bps": total_edge / fill_qty if fill_qty else 0.0,
        "spread_paid_bps": spread_paid_bps,
        "spread_captured_bps": spread_captured_bps,
        "avg_hold_ms": avg_hold_ms,
        "avg_inventory_age_ms": avg_inventory_age_ms,
        "loss_buckets": {
            "fees_usd": fee_drag,
            "slippage_bps": slippage_cost,
            "adverse_selection_bps": adverse_selection_cost,
            "stale_inventory_ratio": stale_inventory / fill_qty if fill_qty else 0.0,
        },
        "venue_contribution": venue_contribution,
        "venue_worst": venue_worst,
    }


def analyse_timestamp_quality(rows: list[dict[str, Any]]) -> dict[str, Any]:
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
            diagnostics["max_exchange_jump_ns"] = max(diagnostics["max_exchange_jump_ns"], abs(exchange_ts - prev_exchange))
            if exchange_ts < prev_exchange:
                diagnostics["exchange_non_monotonic"] += 1
        if prev_local is not None and local_ts > 0:
            diagnostics["max_local_gap_ns"] = max(diagnostics["max_local_gap_ns"], abs(local_ts - prev_local))
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


def analyse_signals(rows: list[dict[str, Any]]) -> dict[str, Any]:
    if not rows:
        return {}
    ordered = sorted(rows, key=lambda r: (int(r.get("event_index", 0)), int(r.get("session_elapsed_ns", 0))))
    raw_sigs = np.nan_to_num(np.array([r.get("signal", 0.0) for r in ordered], dtype=np.float64))
    effective_sigs_bps = np.nan_to_num(np.array([r.get("ret_mid_bps", 0.0) for r in ordered], dtype=np.float64))
    risk = np.nan_to_num(np.array([r.get("risk_score", 0.0) for r in ordered], dtype=np.float64))
    mids = np.nan_to_num(np.array([r.get("mid_price", 0.0) for r in ordered], dtype=np.float64))
    elapsed = np.array([r.get("session_elapsed_ns", 0) for r in ordered], dtype=np.int64)
    signals_for_ic: list[float] = []
    realised_values: list[float] = []
    gating_breakdown = {"confidence_gate": 0, "horizon_disagreement_gate": 0, "safe_mode_gate": 0}
    for record in ordered:
        for reason in record.get("gating_reasons", []):
            if reason in gating_breakdown:
                gating_breakdown[reason] += 1
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
        ic_series = np.array([
            float(np.corrcoef(aligned_sigs[i - _IC_WINDOW : i], realised[i - _IC_WINDOW : i])[0, 1]) if i >= _IC_WINDOW else 0.0
            for i in range(1, len(aligned_sigs) + 1)
        ])
        ic_series = np.nan_to_num(ic_series)
        ic = float(np.mean(ic_series))
        ic_std = float(np.std(ic_series))
        icir = float(ic / ic_std) * math.sqrt(252) if ic_std > 1e-06 else 0.0
    timestamp_quality = analyse_timestamp_quality(ordered)
    duration_min = (int(elapsed[-1]) - int(elapsed[0])) / 1000000000.0 / 60.0 if len(elapsed) > 1 else 0.0
    if timestamp_quality["has_timestamp_issues"]:
        ic = 0.0
        icir = 0.0
    return {
        "total_signals": len(ordered),
        "duration_min": round(duration_min, 2),
        "signal_mean_bps": round(float(np.mean(effective_sigs_bps)), 4),
        "signal_std_bps": round(float(np.std(effective_sigs_bps)), 4),
        "raw_signal_mean_bps": round(float(np.mean(raw_sigs)) * 10000.0, 4),
        "raw_signal_std_bps": round(float(np.std(raw_sigs)) * 10000.0, 4),
        "avg_risk_score": round(float(np.mean(risk)), 4),
        "ic": round(ic, 4),
        "icir_annualised": round(icir, 4),
        "timestamp_quality": timestamp_quality,
        "gating_breakdown": gating_breakdown,
    }


def analyse_ops_events(rows: list[dict[str, Any]]) -> dict[str, Any]:
    training_events = [row for row in rows if row.get("event") in {"bootstrap_training_started", "bootstrap_training_completed", "training_epoch", "continuous_retrain_triggered", "continuous_retrain_completed", "continuous_retrain_failed"}]
    data_events = [row for row in rows if str(row.get("event", "")).startswith("venue_") or row.get("event") == "shadow_health_summary"]
    runtime_events = [row for row in rows if row.get("event") in {"drift_breach", "ensemble_canary_rollback", "safe_mode_activated", "incomplete_model_stack"}]
    per_venue: dict[str, dict[str, int]] = {}
    for row in rows:
        venue = row.get("venue")
        if not isinstance(venue, str):
            continue
        venue = venue.upper()
        if venue not in per_venue:
            per_venue[venue] = {
                "missing_venue_incidents": 0,
                "rest_fallback_usage": 0,
                "resnapshot_count": 0,
            }
        event = row.get("event")
        if event == "venue_rest_fallback_used":
            per_venue[venue]["rest_fallback_usage"] += 1
        elif event == "venue_resnapshot_detected":
            per_venue[venue]["resnapshot_count"] += 1
            per_venue[venue]["missing_venue_incidents"] += 1
    health_events = [row for row in rows if row.get("event") == "shadow_health_summary"]
    health = health_events[-1] if health_events else {}
    return {
        "training_event_count": len(training_events),
        "runtime_event_count": len(runtime_events),
        "data_event_count": len(data_events),
        "drift_breaches": sum(1 for row in rows if row.get("event") == "drift_breach"),
        "canary_rollbacks": sum(1 for row in rows if row.get("event") == "ensemble_canary_rollback"),
        "safe_mode_activations": sum(1 for row in rows if row.get("event") == "safe_mode_activated"),
        "continuous_retrain_failures": sum(1 for row in rows if row.get("event") == "continuous_retrain_failed"),
        "continuous_retrain_completions": sum(1 for row in rows if row.get("event") == "continuous_retrain_completed"),
        "per_venue": per_venue,
        "health": health,
    }


def fill_rate_check(shadow_fill_rate: float, backtest_fill_rate: float, tolerance: float = 0.1) -> dict[str, Any]:
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


def print_report(
    dec: dict[str, Any],
    sig: dict[str, Any],
    ops: dict[str, Any],
    out_path: str | None = None,
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
        f"  Resting (open)         : {dec.get('total_resting', 0)}",
        f"  Fill rate              : {dec.get('fill_rate', 0.0):.2%}",
        f"  Net P&L                : ${dec.get('net_pnl_usd', 0.0):.4f}",
        f"  Fee drag               : ${dec.get('fee_drag_usd', 0.0):.4f}",
        f"  Max drawdown           : ${dec.get('max_drawdown_usd', 0.0):.4f}",
        f"  Total volume (qty)     : {dec.get('total_volume', 0.0):.6f}",
        f"  Impl shortfall         : {dec.get('implementation_shortfall_bps', 0.0):.4f} bps",
        f"  Fill-to-markout        : {dec.get('markout_bps', 0.0):.4f} bps",
        f"  Edge at entry          : {dec.get('edge_at_entry_bps', 0.0):.4f} bps",
        f"  Spread paid/captured   : {dec.get('spread_paid_bps', 0.0):.4f} / {dec.get('spread_captured_bps', 0.0):.4f} bps",
        f"  Avg hold / inv age     : {dec.get('avg_hold_ms', 0.0):.2f} / {dec.get('avg_inventory_age_ms', 0.0):.2f} ms",
        f"  State transitions      : {dec.get('state_transition_count', 0)}",
        f"  Worst venue            : {dec.get('venue_worst', 'UNKNOWN')}",
        "",
    ]
    if dec.get("exchanges"):
        lines.append("── Per-exchange breakdown ───────────────────────────────────")
        for exchange, stats in dec["exchanges"].items():
            maker_pct = stats["maker"] / stats["fills"] if stats["fills"] else 0.0
            lines.append(
                f"  {exchange:10s}  fills={stats['fills']}  maker={maker_pct:.0%}"
                f"  fees=${stats['fees']:.4f}  shortfall={stats['shortfall_bps']:.3f}bps"
                f"  markout={stats['markout_bps']:.3f}bps  hold={stats['hold_ms']:.1f}ms"
                f"  contrib=${stats['net_pnl_usd']:.4f}"
            )
        lines.append("")
    loss_buckets = dec.get("loss_buckets", {})
    lines += [
        "── Loss attribution ────────────────────────────────────────",
        f"  Fees                  : ${loss_buckets.get('fees_usd', 0.0):.4f}",
        f"  Slippage              : {loss_buckets.get('slippage_bps', 0.0):.4f} bps",
        f"  Adverse selection     : {loss_buckets.get('adverse_selection_bps', 0.0):.4f} bps",
        f"  Stale inventory ratio : {loss_buckets.get('stale_inventory_ratio', 0.0):.2%}",
        "",
    ]
    if sig:
        lines += [
            "── Neural alpha signal ──────────────────────────────────────",
            f"  Signals generated      : {sig.get('total_signals', 0)}",
            f"  Session duration       : {sig.get('duration_min', 0):.1f} min",
            f"  Mean effective signal  : {sig.get('signal_mean_bps', 0):.4f} bps",
            f"  Effective signal std   : {sig.get('signal_std_bps', 0):.4f} bps",
            f"  Mean raw signal        : {sig.get('raw_signal_mean_bps', 0):.4f} bps",
            f"  Raw signal std         : {sig.get('raw_signal_std_bps', 0):.4f} bps",
            f"  Avg risk score         : {sig.get('avg_risk_score', 0):.4f}",
            f"  IC (rolling 20)        : {sig.get('ic', 0):.4f}",
            f"  ICIR (annualised)      : {sig.get('icir_annualised', 0):.4f}",
            "",
            "── Gating breakdown ────────────────────────────────────────",
            f"  Confidence gate        : {sig.get('gating_breakdown', {}).get('confidence_gate', 0)}",
            f"  Horizon disagreement   : {sig.get('gating_breakdown', {}).get('horizon_disagreement_gate', 0)}",
            f"  Safe-mode gate         : {sig.get('gating_breakdown', {}).get('safe_mode_gate', 0)}",
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
        "── Shadow health ────────────────────────────────────────────",
        f"  Training events        : {ops.get('training_event_count', 0)}",
        f"  Runtime incidents      : {ops.get('runtime_event_count', 0)}",
        f"  Drift breaches         : {ops.get('drift_breaches', 0)}",
        f"  Canary rollbacks       : {ops.get('canary_rollbacks', 0)}",
        f"  Safe-mode activations  : {ops.get('safe_mode_activations', 0)}",
        f"  Retrain completions    : {ops.get('continuous_retrain_completions', 0)}",
        f"  Retrain failures       : {ops.get('continuous_retrain_failures', 0)}",
        "",
    ]
    per_venue = ops.get("health", {}).get("data_quality", {}).get("per_venue", {}) or ops.get("per_venue", {})
    if per_venue:
        lines.append("── Venue quality ────────────────────────────────────────────")
        for venue, stats in per_venue.items():
            lines.append(
                f"  {venue:10s}  received={stats.get('ticks_received', 0)}  used={stats.get('ticks_used', 0)}"
                f"  missing={stats.get('missing_venue_incidents', 0)}  rest_fallback={stats.get('rest_fallback_usage', 0)}"
                f"  resnapshot={stats.get('resnapshot_count', 0)}"
            )
        lines.append("")
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
        Path(out_path).write_text(report, encoding="utf-8")
        print(f"Report saved → {out_path}")


def write_prometheus(dec: dict[str, Any], sig: dict[str, Any], ops: dict[str, Any], path: str) -> None:
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
        "# HELP shadow_safe_mode_activations Total number of safe mode activations",
        "# TYPE shadow_safe_mode_activations counter",
        f"shadow_safe_mode_activations {ops.get('safe_mode_activations', 0)}",
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
    Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Prometheus metrics → {path}")


def _require_log(path: str, label: str) -> None:
    if not Path(path).exists():
        raise FileNotFoundError(f"Missing required {label} log: {path}")


def main() -> None:
    ap = argparse.ArgumentParser(description="Shadow session metrics rollup")
    ap.add_argument("--decisions", type=str, default=_DEFAULT_DECISIONS_LOG, help="C++ shadow engine decision log")
    ap.add_argument("--signals", type=str, default=_DEFAULT_SIGNALS_LOG, help="Neural alpha signal log")
    ap.add_argument("--ops", type=str, default=_DEFAULT_OPS_LOG, help="Structured ops/runtime log")
    ap.add_argument("--out", type=str, default=None, help="Save text report to file")
    ap.add_argument("--prom", type=str, default=None, help="Save Prometheus metrics to file")
    ap.add_argument("--backtest-fill-rate", type=float, default=None, dest="backtest_fill_rate", help="Expected fill rate from backtest (for readiness check)")
    args = ap.parse_args()
    _require_log(args.decisions, "decisions")
    _require_log(args.ops, "ops")
    dec_rows = load_jsonl(args.decisions)
    sig_rows = load_jsonl(args.signals)
    ops_rows = load_jsonl(args.ops)
    dec = analyse_decisions(dec_rows)
    sig = analyse_signals(sig_rows)
    ops = analyse_ops_events(ops_rows)
    print_report(dec, sig, ops, out_path=args.out)
    if args.backtest_fill_rate is not None:
        check = fill_rate_check(dec.get("fill_rate", 0.0), args.backtest_fill_rate)
        print(f"Fill-rate readiness: shadow={check['shadow_fill_rate']:.2%}  backtest={check['backtest_fill_rate']:.2%}  verdict={check['verdict']}")
    if args.prom:
        write_prometheus(dec, sig, ops, args.prom)


if __name__ == "__main__":
    main()
