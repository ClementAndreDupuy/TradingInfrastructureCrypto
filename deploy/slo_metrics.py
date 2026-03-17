from __future__ import annotations

import json
import os
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
LOG_DIR = Path(os.getenv("SLO_LOG_DIR", ROOT / "logs"))
OUT_DIR = Path(os.getenv("SLO_OUT_DIR", ROOT / "logs"))
OUT_DIR.mkdir(parents=True, exist_ok=True)

WINDOW_DAYS = 28
WINDOW_SECONDS = WINDOW_DAYS * 24 * 60 * 60


def _load_jsonl(path: Path) -> list[dict[str, Any]]:
    if not path.exists():
        return []
    rows: list[dict[str, Any]] = []
    with path.open() as handle:
        for line in handle:
            raw = line.strip()
            if not raw:
                continue
            rows.append(json.loads(raw))
    return rows


def _to_bool(value: Any) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value != 0
    if isinstance(value, str):
        return value.lower() in {"1", "true", "yes", "ok"}
    return False


def _ratio(numerator: float, denominator: float) -> float:
    if denominator <= 0:
        return 1.0
    return max(0.0, min(1.0, numerator / denominator))


def _window_start() -> datetime:
    return datetime.now(timezone.utc) - timedelta(days=WINDOW_DAYS)


def _filter_window(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    cutoff = _window_start().timestamp()
    kept: list[dict[str, Any]] = []
    for row in rows:
        ts = row.get("timestamp") or row.get("timestamp_s")
        if ts is None:
            kept.append(row)
            continue
        if float(ts) >= cutoff:
            kept.append(row)
    return kept


def compute_h4() -> dict[str, float]:
    feed_rows = _filter_window(_load_jsonl(LOG_DIR / "feed_health.jsonl"))
    decision_rows = _filter_window(_load_jsonl(LOG_DIR / "shadow_decisions.jsonl"))
    recon_rows = _filter_window(_load_jsonl(LOG_DIR / "reconciliation_status.jsonl"))
    risk_rows = _filter_window(_load_jsonl(LOG_DIR / "risk_events.jsonl"))

    feed_total = len(feed_rows)
    feed_good = sum(1 for row in feed_rows if float(row.get("staleness_ms", 0.0)) <= 500.0 and int(row.get("sequence_gaps", 0)) == 0)

    terminal = [r for r in decision_rows if r.get("event") in {"FILL", "CANCELED", "REJECTED"}]
    reject_count = sum(1 for row in terminal if row.get("event") == "REJECTED")

    recon_total = len(recon_rows)
    recon_good = sum(1 for row in recon_rows if _to_bool(row.get("drift_ok", True)))

    risk_total = len(risk_rows)
    risk_safe = sum(1 for row in risk_rows if not _to_bool(row.get("triggered", False)))

    return {
        "trading_sli_feed_integrity": _ratio(feed_good, feed_total),
        "trading_sli_reject_health": _ratio(max(0, len(terminal) - reject_count), len(terminal)),
        "trading_sli_reconciliation_health": _ratio(recon_good, recon_total),
        "trading_sli_risk_health": _ratio(risk_safe, risk_total),
    }


def compute_h6() -> dict[str, float]:
    db_rows = _filter_window(_load_jsonl(LOG_DIR / "db_health.jsonl"))

    ingest_good = 0
    write_good = 0
    complete_good = 0
    correct_good = 0
    durable_good = 0
    lineage_good = 0

    for row in db_rows:
        ingest_good += 1 if float(row.get("ingest_freshness_s", 0.0)) <= 30.0 else 0
        write_good += 1 if _to_bool(row.get("write_ok", True)) else 0

        expected = float(row.get("expected_records", 0.0))
        actual = float(row.get("actual_records", expected))
        completeness = _ratio(actual, expected)
        complete_good += 1 if completeness >= 0.999 else 0

        correct_good += 1 if _to_bool(row.get("checksum_ok", True)) else 0
        durable_good += 1 if _to_bool(row.get("durability_ok", True)) else 0
        lineage_good += 1 if _to_bool(row.get("lineage_ok", True)) else 0

    total = len(db_rows)
    return {
        "db_sli_ingest_freshness": _ratio(ingest_good, total),
        "db_sli_write_availability": _ratio(write_good, total),
        "db_sli_completeness": _ratio(complete_good, total),
        "db_sli_correctness": _ratio(correct_good, total),
        "db_sli_durability": _ratio(durable_good, total),
        "db_sli_lineage": _ratio(lineage_good, total),
    }


def _error_budget_remaining(sli: float, target: float) -> float:
    budget = 1.0 - target
    if budget <= 0:
        return 0.0
    consumed = max(0.0, target - sli)
    return max(0.0, 1.0 - (consumed / budget))


def _burn_rate(sli: float, target: float) -> float:
    budget = 1.0 - target
    if budget <= 0:
        return 0.0
    bad = max(0.0, 1.0 - sli)
    return bad / budget


def build_scorecard() -> dict[str, Any]:
    h4 = compute_h4()
    h6 = compute_h6()

    targets = {
        "trading_sli_feed_integrity": 0.999,
        "trading_sli_reject_health": 0.995,
        "trading_sli_reconciliation_health": 0.999,
        "trading_sli_risk_health": 0.9995,
        "db_sli_ingest_freshness": 0.999,
        "db_sli_write_availability": 0.9995,
        "db_sli_completeness": 0.999,
        "db_sli_correctness": 0.9999,
        "db_sli_durability": 0.9999,
        "db_sli_lineage": 0.999,
    }

    all_metrics = {**h4, **h6}
    scorecard = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "window_days": WINDOW_DAYS,
        "sli": all_metrics,
        "targets": targets,
        "error_budget_remaining": {
            key: _error_budget_remaining(value, targets[key]) for key, value in all_metrics.items()
        },
        "burn_rate": {
            key: _burn_rate(value, targets[key]) for key, value in all_metrics.items()
        },
    }
    return scorecard


def _write_outputs(scorecard: dict[str, Any]) -> None:
    date = datetime.now(timezone.utc).strftime("%Y%m%d")
    json_path = OUT_DIR / f"slo_scorecard_{date}.json"
    with json_path.open("w") as handle:
        json.dump(scorecard, handle, indent=2)

    prom_path = OUT_DIR / "slo_metrics.prom"
    with prom_path.open("w") as handle:
        handle.write("# TYPE trading_sli_feed_integrity gauge\n")
        for key, value in scorecard["sli"].items():
            handle.write(f"{key} {value:.6f}\n")
        for key, value in scorecard["error_budget_remaining"].items():
            handle.write(f"{key}_error_budget_remaining {value:.6f}\n")
        for key, value in scorecard["burn_rate"].items():
            handle.write(f"{key}_burn_rate {value:.6f}\n")


if __name__ == "__main__":
    report = build_scorecard()
    _write_outputs(report)
    print(json.dumps(report, indent=2))
