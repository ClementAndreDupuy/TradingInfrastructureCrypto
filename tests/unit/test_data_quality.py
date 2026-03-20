"""
Unit tests for research.neural_alpha.operations.data_quality.

Covers all five quality gates:
    1. schema_validation
    2. null_rate_bounds
    3. sequence_gap_check
    4. timestamp_skew
    5. duplicate_detection
"""

from __future__ import annotations

import json
import sys
import time
from pathlib import Path

import polars as pl
import pytest

ROOT = Path(__file__).parent.parent.parent
sys.path.insert(0, str(ROOT))

from research.neural_alpha.operations.data_quality import (
    DataQualityConfig,
    DataQualityError,
    QualityReport,
    assert_quality_passed,
    run_quality_gates,
    write_quality_report,
)

# ── Helpers ────────────────────────────────────────────────────────────────────

N_LEVELS = 5


def _lob_columns() -> dict:
    """Return a minimal valid row dict for a single LOB snapshot."""
    row: dict = {}
    for i in range(1, N_LEVELS + 1):
        row[f"bid_price_{i}"] = 100.0 - i * 0.01
        row[f"bid_size_{i}"] = float(10 * i)
        row[f"ask_price_{i}"] = 100.0 + i * 0.01
        row[f"ask_size_{i}"] = float(10 * i)
    return row


def _make_df(
    n: int = 50,
    exchange: str = "BINANCE",
    symbol: str = "SOLUSDT",
    base_ts_ns: int | None = None,
    gap_ns: int = 1_000_000_000,  # 1 second
) -> pl.DataFrame:
    """Build a minimal valid LOB DataFrame with *n* rows."""
    if base_ts_ns is None:
        base_ts_ns = time.time_ns() - n * gap_ns
    rows = []
    for i in range(n):
        row = {
            "timestamp_ns": base_ts_ns + i * gap_ns,
            "exchange": exchange,
            "symbol": symbol,
            "best_bid": 100.0 - 0.01,
            "best_ask": 100.0 + 0.01,
            **_lob_columns(),
        }
        rows.append(row)
    return pl.DataFrame(rows)


_CFG_PERMISSIVE = DataQualityConfig(
    max_null_rate=0.05,
    max_gap_ns=300 * 1_000_000_000,
    max_age_ns=26 * 3_600 * 1_000_000_000,
    allow_future_ticks=False,
    future_tolerance_ns=60 * 1_000_000_000,
)


# ── Schema validation ──────────────────────────────────────────────────────────


def test_schema_passes_on_valid_df():
    df = _make_df()
    report = run_quality_gates(df, _CFG_PERMISSIVE)
    schema_check = next(c for c in report.checks if c.check == "schema_validation")
    assert schema_check.passed


def test_schema_fails_on_missing_column():
    df = _make_df().drop("best_bid")
    report = run_quality_gates(df, _CFG_PERMISSIVE)
    schema_check = next(c for c in report.checks if c.check == "schema_validation")
    assert not schema_check.passed
    assert "best_bid" in schema_check.breach_reason
    assert not report.passed


def test_schema_fails_on_wrong_dtype():
    df = _make_df().with_columns(pl.col("timestamp_ns").cast(pl.Float64))
    report = run_quality_gates(df, _CFG_PERMISSIVE)
    schema_check = next(c for c in report.checks if c.check == "schema_validation")
    assert not schema_check.passed


# ── Null-rate bounds ───────────────────────────────────────────────────────────


def test_null_rate_passes_on_clean_df():
    df = _make_df()
    report = run_quality_gates(df, _CFG_PERMISSIVE)
    null_check = next(c for c in report.checks if c.check == "null_rate_bounds")
    assert null_check.passed


def test_null_rate_fails_when_too_many_nulls():
    df = _make_df(n=20)
    # Replace half of best_bid with nulls
    nulled = [None if i % 2 == 0 else 99.99 for i in range(len(df))]
    df = df.with_columns(pl.Series("best_bid", nulled, dtype=pl.Float64))
    cfg = DataQualityConfig(max_null_rate=0.1)
    report = run_quality_gates(df, cfg)
    null_check = next(c for c in report.checks if c.check == "null_rate_bounds")
    assert not null_check.passed
    assert "best_bid" in null_check.breach_reason


def test_null_rate_passes_below_threshold():
    df = _make_df(n=100)
    # Introduce 2 nulls out of 100 rows → 2 % < default 5 %
    vals = df["best_bid"].to_list()
    vals[0] = None
    vals[1] = None
    df = df.with_columns(pl.Series("best_bid", vals, dtype=pl.Float64))
    report = run_quality_gates(df, _CFG_PERMISSIVE)
    null_check = next(c for c in report.checks if c.check == "null_rate_bounds")
    assert null_check.passed


# ── Sequence-gap check ─────────────────────────────────────────────────────────


def test_gap_check_passes_on_regular_ticks():
    df = _make_df(gap_ns=1_000_000_000)  # 1-second gaps
    report = run_quality_gates(df, _CFG_PERMISSIVE)
    gap_check = next(c for c in report.checks if c.check == "sequence_gap_check")
    assert gap_check.passed


def test_gap_check_fails_on_large_gap():
    base = time.time_ns() - 3_600 * 1_000_000_000
    rows = []
    for i in range(30):
        gap = 1_000_000_000 if i != 15 else 600 * 1_000_000_000  # 10-min gap at tick 15
        ts = base + i * 1_000_000_000 + (0 if i <= 15 else (600 - 1) * 1_000_000_000)
        rows.append(
            {
                "timestamp_ns": base
                + sum((600 * 1_000_000_000 if j == 15 else 1_000_000_000) for j in range(i)),
                "exchange": "BINANCE",
                "symbol": "SOLUSDT",
                "best_bid": 99.99,
                "best_ask": 100.01,
                **_lob_columns(),
            }
        )
    df = pl.DataFrame(rows)
    cfg = DataQualityConfig(max_gap_ns=300 * 1_000_000_000)  # 5-min limit
    report = run_quality_gates(df, cfg)
    gap_check = next(c for c in report.checks if c.check == "sequence_gap_check")
    assert not gap_check.passed
    assert "BINANCE" in gap_check.affected_venues


def test_gap_check_skipped_if_single_tick():
    df = _make_df(n=1)
    report = run_quality_gates(df, _CFG_PERMISSIVE)
    gap_check = next(c for c in report.checks if c.check == "sequence_gap_check")
    assert gap_check.passed  # Nothing to compare → passes


# ── Timestamp skew ─────────────────────────────────────────────────────────────


def test_timestamp_skew_passes_on_fresh_ticks():
    df = _make_df()  # timestamps close to now
    report = run_quality_gates(df, _CFG_PERMISSIVE)
    ts_check = next(c for c in report.checks if c.check == "timestamp_skew")
    assert ts_check.passed


def test_timestamp_skew_fails_on_old_data():
    old_ns = time.time_ns() - 48 * 3_600 * 1_000_000_000  # 48 hours ago
    df = _make_df(base_ts_ns=old_ns)
    cfg = DataQualityConfig(max_age_ns=26 * 3_600 * 1_000_000_000)
    report = run_quality_gates(df, cfg)
    ts_check = next(c for c in report.checks if c.check == "timestamp_skew")
    assert not ts_check.passed
    assert "old" in ts_check.breach_reason.lower()


def test_timestamp_skew_fails_on_future_ticks():
    future_ns = time.time_ns() + 10 * 60 * 1_000_000_000  # 10 minutes in future
    df = _make_df(base_ts_ns=future_ns)
    cfg = DataQualityConfig(allow_future_ticks=False, future_tolerance_ns=60 * 1_000_000_000)
    report = run_quality_gates(df, cfg)
    ts_check = next(c for c in report.checks if c.check == "timestamp_skew")
    assert not ts_check.passed
    assert "future" in ts_check.breach_reason.lower()


def test_timestamp_skew_allows_future_when_configured():
    future_ns = time.time_ns() + 10 * 60 * 1_000_000_000
    df = _make_df(base_ts_ns=future_ns)
    cfg = DataQualityConfig(allow_future_ticks=True)
    report = run_quality_gates(df, cfg)
    ts_check = next(c for c in report.checks if c.check == "timestamp_skew")
    assert ts_check.passed


# ── Duplicate detection ────────────────────────────────────────────────────────


def test_duplicate_check_passes_on_unique_rows():
    df = _make_df()
    report = run_quality_gates(df, _CFG_PERMISSIVE)
    dup_check = next(c for c in report.checks if c.check == "duplicate_detection")
    assert dup_check.passed


def test_duplicate_check_fails_on_duplicate_rows():
    df = _make_df(n=10)
    # Stack the first row to create duplicates
    df_with_dupes = pl.concat([df, df[:2]])
    report = run_quality_gates(df_with_dupes, _CFG_PERMISSIVE)
    dup_check = next(c for c in report.checks if c.check == "duplicate_detection")
    assert not dup_check.passed
    assert dup_check.details["n_duplicates"] == 2
    assert "BINANCE" in dup_check.affected_venues


# ── assert_quality_passed / DataQualityError ──────────────────────────────────


def test_assert_passes_on_clean_df():
    df = _make_df()
    report = run_quality_gates(df, _CFG_PERMISSIVE)
    assert_quality_passed(report)  # should not raise


def test_assert_raises_data_quality_error_on_breach():
    df = _make_df().drop("best_bid")
    report = run_quality_gates(df, _CFG_PERMISSIVE)
    with pytest.raises(DataQualityError) as exc_info:
        assert_quality_passed(report)
    assert exc_info.value.report is report


# ── write_quality_report ───────────────────────────────────────────────────────


def test_write_quality_report_produces_valid_json(tmp_path):
    df = _make_df()
    report = run_quality_gates(df, _CFG_PERMISSIVE)
    out = tmp_path / "quality_report.json"
    write_quality_report(report, out)
    assert out.exists()
    with open(out) as f:
        data = json.load(f)
    assert "passed" in data
    assert "checks" in data
    assert isinstance(data["checks"], list)
    assert len(data["checks"]) == 5  # one per gate


def test_write_quality_report_includes_breach_reasons(tmp_path):
    df = _make_df().drop("timestamp_ns")
    report = run_quality_gates(df, _CFG_PERMISSIVE)
    out = tmp_path / "breach_report.json"
    write_quality_report(report, out)
    with open(out) as f:
        data = json.load(f)
    assert not data["passed"]
    schema_check = next(c for c in data["checks"] if c["check"] == "schema_validation")
    assert not schema_check["passed"]
    assert schema_check["breach_reason"]
