from research.backtest.shadow_metrics import analyse_decisions


def test_analyse_decisions_produces_phase_zero_attribution() -> None:
    rows = [
        {
            "event": "FILL",
            "exchange": "BINANCE",
            "side": "BID",
            "maker": False,
            "fill_px": 100.2,
            "qty": 1.0,
            "fee_usd": 0.05,
            "cumulative_pnl": -100.25,
            "implementation_shortfall_bps": 2.0,
            "markout_bps": -3.0,
            "edge_at_entry_bps": 5.0,
            "hold_time_ms": 200.0,
            "decision_mid": 100.0,
            "ts_ns": 1_000_000_000,
        },
        {
            "event": "FILL",
            "exchange": "OKX",
            "side": "ASK",
            "maker": True,
            "fill_px": 100.1,
            "qty": 1.0,
            "fee_usd": 0.02,
            "cumulative_pnl": -0.17,
            "implementation_shortfall_bps": -1.0,
            "markout_bps": -2.5,
            "edge_at_entry_bps": 4.5,
            "hold_time_ms": 800.0,
            "decision_mid": 100.0,
            "ts_ns": 2_000_000_000,
        },
    ]

    report = analyse_decisions(rows)

    assert report["total_fills"] == 2
    assert report["fees_usd"] == 0.07
    assert report["venue_worst"] in {"BINANCE", "OKX"}
    assert report["avg_hold_ms"] == 500.0
    assert "fees_usd" in report["loss_buckets"]
    assert "slippage_bps" in report["loss_buckets"]
    assert "adverse_selection_bps" in report["loss_buckets"]
    assert report["exchanges"]["BINANCE"]["shortfall_bps"] == 2.0
    assert report["exchanges"]["OKX"]["markout_bps"] == -2.5


def test_analyse_decisions_explains_venue_priority_shifts() -> None:
    rows = [
        {
            "event": "CANCELED",
            "exchange": "BINANCE",
            "hold_time_ms": 900.0,
            "ts_ns": 1,
        },
        {
            "event": "FILL",
            "exchange": "BINANCE",
            "maker": False,
            "markout_bps": -2.0,
            "fill_px": 100.0,
            "qty": 1.0,
            "fee_usd": 0.01,
            "implementation_shortfall_bps": 1.0,
            "edge_at_entry_bps": 2.0,
            "hold_time_ms": 50.0,
            "decision_mid": 100.0,
            "ts_ns": 2,
        },
        {
            "event": "CANCELED",
            "exchange": "KRAKEN",
            "hold_time_ms": 600.0,
            "ts_ns": 3,
        },
        {
            "event": "FILL",
            "exchange": "KRAKEN",
            "maker": True,
            "markout_bps": -1.5,
            "fill_px": 100.0,
            "qty": 1.0,
            "fee_usd": 0.01,
            "implementation_shortfall_bps": 0.5,
            "edge_at_entry_bps": 2.0,
            "hold_time_ms": 60.0,
            "decision_mid": 100.0,
            "ts_ns": 7,
        },
        {
            "event": "FILL",
            "exchange": "KRAKEN",
            "maker": True,
            "markout_bps": 1.2,
            "fill_px": 100.0,
            "qty": 1.0,
            "fee_usd": 0.01,
            "implementation_shortfall_bps": 0.2,
            "edge_at_entry_bps": 2.5,
            "hold_time_ms": 70.0,
            "decision_mid": 100.0,
            "ts_ns": 4,
        },
        {
            "event": "FILL",
            "exchange": "BINANCE",
            "maker": False,
            "markout_bps": -2.5,
            "fill_px": 100.0,
            "qty": 1.0,
            "fee_usd": 0.01,
            "implementation_shortfall_bps": 1.5,
            "edge_at_entry_bps": 2.0,
            "hold_time_ms": 55.0,
            "decision_mid": 100.0,
            "ts_ns": 5,
        },
        {
            "event": "FILL",
            "exchange": "KRAKEN",
            "maker": True,
            "markout_bps": 1.5,
            "fill_px": 100.0,
            "qty": 1.0,
            "fee_usd": 0.01,
            "implementation_shortfall_bps": 0.1,
            "edge_at_entry_bps": 2.5,
            "hold_time_ms": 80.0,
            "decision_mid": 100.0,
            "ts_ns": 6,
        },
    ]

    report = analyse_decisions(rows)
    priority = report["venue_priority"]

    assert priority["venues"]["KRAKEN"]["score"] > priority["venues"]["BINANCE"]["score"]
    assert priority["venues"]["KRAKEN"]["score_delta"] > 0.0
    assert priority["venues"]["BINANCE"]["score_delta"] > 0.0
    assert "positive passive markout" in priority["venues"]["KRAKEN"]["reason"]
    assert "slow cancels" in priority["venues"]["BINANCE"]["reason"]
