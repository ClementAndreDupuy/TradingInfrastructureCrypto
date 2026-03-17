"""
Data quality gates for neural alpha training pipeline.

Validates a LOB snapshot DataFrame before training and model promotion.
All checks are configurable via DataQualityConfig. The job fails closed:
any breach raises DataQualityError, which the caller must explicitly handle.

Gates:
    1. Schema validation   — required columns present with correct dtype families
    2. Null-rate bounds    — per-column null fraction ≤ max_null_rate
    3. Sequence-gap check  — inter-tick timestamp gaps ≤ max_gap_ns per venue
    4. Timestamp skew      — no tick in the future, no tick older than max_age_ns
    5. Duplicate detection — no (timestamp_ns, exchange) duplicates
"""
from __future__ import annotations

import json
import logging
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import polars as pl

log = logging.getLogger(__name__)

# ── Schema ─────────────────────────────────────────────────────────────────────

N_LEVELS = 5

# (column_name, expected dtype category: "int", "float", "str")
_REQUIRED_COLUMNS: list[tuple[str, str]] = [
    ("timestamp_ns", "int"),
    ("exchange", "str"),
    ("symbol", "str"),
    ("best_bid", "float"),
    ("best_ask", "float"),
    *[(f"bid_price_{i}", "float") for i in range(1, N_LEVELS + 1)],
    *[(f"bid_size_{i}", "float") for i in range(1, N_LEVELS + 1)],
    *[(f"ask_price_{i}", "float") for i in range(1, N_LEVELS + 1)],
    *[(f"ask_size_{i}", "float") for i in range(1, N_LEVELS + 1)],
]

_FLOAT_DTYPES = {pl.Float32, pl.Float64}
_INT_DTYPES = {pl.Int8, pl.Int16, pl.Int32, pl.Int64, pl.UInt8, pl.UInt16, pl.UInt32, pl.UInt64}
_STR_DTYPES = {pl.Utf8, pl.String}


# ── Config ─────────────────────────────────────────────────────────────────────

@dataclass
class DataQualityConfig:
    # Maximum null fraction allowed per column (0.05 = 5 %)
    max_null_rate: float = 0.05
    # Maximum allowed gap between consecutive ticks per venue (nanoseconds)
    # Default: 5 minutes
    max_gap_ns: int = 5 * 60 * 1_000_000_000
    # Maximum age of the oldest tick relative to now (nanoseconds)
    # Default: 26 hours (to accommodate daily jobs)
    max_age_ns: int = 26 * 3_600 * 1_000_000_000
    # Whether to allow any ticks with timestamp_ns in the future
    allow_future_ticks: bool = False
    # Tolerance for "future" check (nanoseconds) — accounts for clock skew
    future_tolerance_ns: int = 60 * 1_000_000_000  # 1 minute


# ── Result types ───────────────────────────────────────────────────────────────

@dataclass
class CheckResult:
    check: str
    passed: bool
    breach_reason: str = ""
    affected_venues: list[str] = field(default_factory=list)
    affected_symbols: list[str] = field(default_factory=list)
    details: dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> dict[str, Any]:
        return {
            "check": self.check,
            "passed": self.passed,
            "breach_reason": self.breach_reason,
            "affected_venues": self.affected_venues,
            "affected_symbols": self.affected_symbols,
            "details": self.details,
        }


@dataclass
class QualityReport:
    passed: bool
    run_timestamp_ns: int
    n_rows: int
    checks: list[CheckResult]
    config: dict[str, Any]

    def to_dict(self) -> dict[str, Any]:
        return {
            "passed": self.passed,
            "run_timestamp_ns": self.run_timestamp_ns,
            "n_rows": self.n_rows,
            "checks": [c.to_dict() for c in self.checks],
            "config": self.config,
        }

    def log_summary(self) -> None:
        status = "PASSED" if self.passed else "FAILED"
        log.info("[DataQuality] Overall: %s  rows=%d", status, self.n_rows)
        for c in self.checks:
            lvl = logging.INFO if c.passed else logging.ERROR
            log.log(
                lvl,
                "[DataQuality] %-28s %s%s",
                c.check,
                "OK" if c.passed else "BREACH",
                f" — {c.breach_reason}" if c.breach_reason else "",
            )


class DataQualityError(RuntimeError):
    """Raised when one or more data quality gates breach their thresholds."""

    def __init__(self, report: QualityReport) -> None:
        self.report = report
        breaches = [c.check for c in report.checks if not c.passed]
        super().__init__(
            f"Data quality gates failed: {breaches}. "
            "Inspect the quality report for details."
        )


# ── Individual checks ──────────────────────────────────────────────────────────

def _check_schema(df: pl.DataFrame) -> CheckResult:
    """Verify all required columns exist with compatible dtypes."""
    missing: list[str] = []
    wrong_type: list[str] = []

    for col, expected_kind in _REQUIRED_COLUMNS:
        if col not in df.columns:
            missing.append(col)
            continue
        dtype = df[col].dtype
        if expected_kind == "float" and dtype not in _FLOAT_DTYPES:
            wrong_type.append(f"{col}({dtype})")
        elif expected_kind == "int" and dtype not in _INT_DTYPES:
            wrong_type.append(f"{col}({dtype})")
        elif expected_kind == "str" and dtype not in _STR_DTYPES:
            wrong_type.append(f"{col}({dtype})")

    passed = not missing and not wrong_type
    reason = ""
    if missing:
        reason += f"missing columns: {missing}. "
    if wrong_type:
        reason += f"wrong dtype: {wrong_type}."
    return CheckResult(
        check="schema_validation",
        passed=passed,
        breach_reason=reason.strip(),
        details={"missing_columns": missing, "wrong_dtype_columns": wrong_type},
    )


def _check_null_rates(df: pl.DataFrame, max_null_rate: float) -> CheckResult:
    """Ensure no column exceeds the maximum allowed null fraction."""
    breaching: dict[str, float] = {}
    for col, _ in _REQUIRED_COLUMNS:
        if col not in df.columns:
            continue
        null_frac = df[col].null_count() / max(len(df), 1)
        if null_frac > max_null_rate:
            breaching[col] = round(null_frac, 6)

    passed = len(breaching) == 0
    reason = f"columns exceed null-rate {max_null_rate}: {breaching}" if not passed else ""
    return CheckResult(
        check="null_rate_bounds",
        passed=passed,
        breach_reason=reason,
        details={"breaching_columns": breaching, "max_null_rate": max_null_rate},
    )


def _check_sequence_gaps(df: pl.DataFrame, max_gap_ns: int) -> CheckResult:
    """Detect per-venue timestamp gaps larger than max_gap_ns."""
    if "timestamp_ns" not in df.columns or "exchange" not in df.columns:
        return CheckResult(
            check="sequence_gap_check",
            passed=True,
            breach_reason="skipped (missing timestamp_ns or exchange column)",
        )

    breaching_venues: list[str] = []
    max_observed: dict[str, int] = {}

    for venue in df["exchange"].unique().to_list():
        venue_df = df.filter(pl.col("exchange") == venue).sort("timestamp_ns")
        ts = venue_df["timestamp_ns"].to_numpy()
        if len(ts) < 2:
            continue
        gaps = ts[1:] - ts[:-1]
        max_gap = int(gaps.max())
        max_observed[venue] = max_gap
        if max_gap > max_gap_ns:
            breaching_venues.append(venue)

    passed = len(breaching_venues) == 0
    reason = (
        f"venues with gap > {max_gap_ns}ns: {breaching_venues}" if not passed else ""
    )
    return CheckResult(
        check="sequence_gap_check",
        passed=passed,
        breach_reason=reason,
        affected_venues=breaching_venues,
        details={
            "max_gap_ns_threshold": max_gap_ns,
            "max_gap_ns_observed_per_venue": max_observed,
        },
    )


def _check_timestamp_skew(
    df: pl.DataFrame,
    max_age_ns: int,
    allow_future_ticks: bool,
    future_tolerance_ns: int,
) -> CheckResult:
    """Check timestamps are not too old and not meaningfully in the future."""
    if "timestamp_ns" not in df.columns:
        return CheckResult(
            check="timestamp_skew",
            passed=True,
            breach_reason="skipped (missing timestamp_ns column)",
        )

    now_ns = time.time_ns()
    ts = df["timestamp_ns"]
    min_ts = int(ts.min())  # type: ignore[arg-type]
    max_ts = int(ts.max())  # type: ignore[arg-type]

    oldest_age_ns = now_ns - min_ts
    future_skew_ns = max_ts - now_ns  # positive → in future

    too_old = oldest_age_ns > max_age_ns
    too_future = (not allow_future_ticks) and (future_skew_ns > future_tolerance_ns)

    passed = not too_old and not too_future
    reason_parts: list[str] = []
    if too_old:
        reason_parts.append(
            f"oldest tick is {oldest_age_ns / 1e9:.1f}s old (limit {max_age_ns / 1e9:.1f}s)"
        )
    if too_future:
        reason_parts.append(
            f"newest tick is {future_skew_ns / 1e9:.1f}s in the future "
            f"(tolerance {future_tolerance_ns / 1e9:.1f}s)"
        )

    return CheckResult(
        check="timestamp_skew",
        passed=passed,
        breach_reason="; ".join(reason_parts),
        details={
            "now_ns": now_ns,
            "min_timestamp_ns": min_ts,
            "max_timestamp_ns": max_ts,
            "oldest_age_ns": oldest_age_ns,
            "future_skew_ns": future_skew_ns,
            "max_age_ns": max_age_ns,
            "future_tolerance_ns": future_tolerance_ns,
        },
    )


def _check_duplicates(df: pl.DataFrame) -> CheckResult:
    """Detect duplicate (timestamp_ns, exchange) rows."""
    key_cols = [c for c in ("timestamp_ns", "exchange") if c in df.columns]
    if not key_cols:
        return CheckResult(
            check="duplicate_detection",
            passed=True,
            breach_reason="skipped (no key columns available)",
        )

    n_total = len(df)
    n_unique = df.select(key_cols).n_unique()
    n_dupes = n_total - n_unique

    affected_venues: list[str] = []
    if n_dupes > 0 and "exchange" in df.columns:
        # Find which venues have dupes
        dup_mask = df.select(key_cols).is_duplicated()
        affected_venues = (
            df.filter(dup_mask)["exchange"].unique().to_list()
            if "exchange" in df.columns
            else []
        )

    passed = n_dupes == 0
    reason = f"{n_dupes} duplicate (timestamp_ns, exchange) rows found" if not passed else ""
    return CheckResult(
        check="duplicate_detection",
        passed=passed,
        breach_reason=reason,
        affected_venues=affected_venues,
        details={"n_total": n_total, "n_unique": n_unique, "n_duplicates": n_dupes},
    )


# ── Public API ─────────────────────────────────────────────────────────────────

def run_quality_gates(
    df: pl.DataFrame,
    cfg: DataQualityConfig | None = None,
) -> QualityReport:
    """
    Run all data quality gates against *df*.

    Returns a QualityReport. Does NOT raise — callers decide whether to raise
    based on report.passed (use assert_quality_passed for fail-closed behaviour).
    """
    cfg = cfg or DataQualityConfig()
    checks: list[CheckResult] = [
        _check_schema(df),
        _check_null_rates(df, cfg.max_null_rate),
        _check_sequence_gaps(df, cfg.max_gap_ns),
        _check_timestamp_skew(
            df,
            cfg.max_age_ns,
            cfg.allow_future_ticks,
            cfg.future_tolerance_ns,
        ),
        _check_duplicates(df),
    ]
    passed = all(c.passed for c in checks)
    report = QualityReport(
        passed=passed,
        run_timestamp_ns=time.time_ns(),
        n_rows=len(df),
        checks=checks,
        config={
            "max_null_rate": cfg.max_null_rate,
            "max_gap_ns": cfg.max_gap_ns,
            "max_age_ns": cfg.max_age_ns,
            "allow_future_ticks": cfg.allow_future_ticks,
            "future_tolerance_ns": cfg.future_tolerance_ns,
        },
    )
    report.log_summary()
    return report


def assert_quality_passed(report: QualityReport) -> None:
    """Raise DataQualityError if the report contains any breaches."""
    if not report.passed:
        raise DataQualityError(report)


def write_quality_report(report: QualityReport, path: Path) -> None:
    """Persist the quality report as JSON to *path*."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w") as f:
        json.dump(report.to_dict(), f, indent=2)
    log.info("[DataQuality] Report written → %s", path)
