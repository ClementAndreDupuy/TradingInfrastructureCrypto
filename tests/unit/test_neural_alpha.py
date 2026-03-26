"""
Unit tests for the neural alpha pipeline.

Tests:
    - Feature engineering shapes and value correctness
    - Model forward pass shapes and gradients
    - Loss computation
    - Dataset windowing
    - Backtest logic
    - Alpha regression metrics
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import polars as pl
import pytest
import torch

# Make sure the project root is on sys.path
ROOT = Path(__file__).parent.parent.parent
sys.path.insert(0, str(ROOT))

from research.neural_alpha.evaluation.alpha_regression import (
    analyse_alpha,
    analyse_direction_calibration,
    analyse_flat_threshold_sensitivity,
    compute_hit_rate,
    compute_ic,
    compute_turnover,
    expected_calibration_error,
    ols_regression,
    sweep_direction_thresholds,
)
from research.neural_alpha.evaluation.backtest import BacktestConfig, NeuralAlphaBacktest
from research.neural_alpha.data.dataset import (
    DatasetConfig,
    LOBDataset,
    rolling_normalise,
    split_train_validation,
    split_walk_forward,
)
from research.neural_alpha.data.features import (
    compute_labels,
    compute_lob_tensor,
    compute_scalar_features,
    normalise_scalar,
    D_SCALAR,
    N_LEVELS,
    TRADE_FLOW_FEATURE_INDICES,
)
from research.neural_alpha.models.model import (
    CryptoAlphaNet,
    LOBSpatialEncoder,
    MultiTaskLoss,
    TemporalEncoder,
)
from research.neural_alpha.models.trainer import TrainerConfig, walk_forward_train
from research.neural_alpha.runtime.shadow_session import (
    NeuralAlphaShadowSession,
    ShadowSessionConfig,
    _ensure_trade_flow_schema,
    _build_signal_alignment,
    _summarise_regime_churn,
    _summarise_timestamp_quality,
    _symbol_model_path,
)
from research.backtest.shadow_metrics import analyse_ops_events, analyse_signals


# ── Test data helpers ─────────────────────────────────────────────────────────


def _make_lob_df(n_ticks: int, seed: int = 0) -> pl.DataFrame:
    """Build a minimal L5 LOB DataFrame for unit tests."""
    rng = np.random.default_rng(seed)
    mid = 50_000.0
    rows = []
    for t in range(n_ticks):
        mid += rng.normal(0, 5)
        spread = abs(rng.normal(10, 3)) + 1.0
        row: dict = {
            "timestamp_ns": int(1_700_000_000_000_000_000 + t * 500_000_000),
            "exchange": "BINANCE",
            "best_bid": mid - spread / 2,
            "best_ask": mid + spread / 2,
            "last_trade_price": mid,
            "last_trade_size": float(abs(rng.normal(0.2, 0.05))),
            "recent_traded_volume": float(abs(rng.normal(1.0, 0.2))),
            "trade_direction": 1 if t % 2 == 0 else 0,
        }
        for i in range(1, 6):
            row[f"bid_price_{i}"] = mid - spread / 2 - (i - 1) * spread
            row[f"bid_size_{i}"] = float(rng.lognormal(1, 0.5))
            row[f"ask_price_{i}"] = mid + spread / 2 + (i - 1) * spread
            row[f"ask_size_{i}"] = float(rng.lognormal(1, 0.5))
        rows.append(row)
    return pl.DataFrame(rows)


# ── Fixtures ──────────────────────────────────────────────────────────────────


@pytest.fixture
def small_df() -> pl.DataFrame:
    return _make_lob_df(n_ticks=200, seed=0)


@pytest.fixture
def medium_df() -> pl.DataFrame:
    return _make_lob_df(n_ticks=500, seed=1)


# ── Feature engineering ───────────────────────────────────────────────────────


class TestFeatures:
    def test_lob_tensor_shape(self, small_df: pl.DataFrame) -> None:
        lob = compute_lob_tensor(small_df)
        assert lob.shape == (200, 5, 4), f"Expected (200, 5, 4), got {lob.shape}"

    def test_lob_tensor_dtype(self, small_df: pl.DataFrame) -> None:
        lob = compute_lob_tensor(small_df)
        assert lob.dtype == np.float32

    def test_scalar_feature_shape(self, small_df: pl.DataFrame) -> None:
        scalar = compute_scalar_features(small_df)
        assert scalar.shape == (200, D_SCALAR)

    def test_scalar_no_inf(self, small_df: pl.DataFrame) -> None:
        scalar = compute_scalar_features(small_df)
        assert np.isfinite(scalar).all(), "Scalar features contain inf/nan"

    def test_queue_imbalance_feature_count_matches_levels(self, small_df: pl.DataFrame) -> None:
        scalar = compute_scalar_features(small_df)
        qi_start = 13
        qi_end = qi_start + N_LEVELS
        qi = scalar[:, qi_start:qi_end]
        assert qi.shape == (200, N_LEVELS)

    def test_labels_shape(self, small_df: pl.DataFrame) -> None:
        labels = compute_labels(small_df)
        assert labels.shape == (200, 6)

    def test_labels_direction_valid(self, small_df: pl.DataFrame) -> None:
        labels = compute_labels(small_df)
        direction = labels[:, 4]
        assert set(direction.astype(int).tolist()).issubset({0, 1, 2})

    def test_labels_adv_sel_binary(self, small_df: pl.DataFrame) -> None:
        labels = compute_labels(small_df)
        adv_sel = labels[:, 5]
        assert set(adv_sel.astype(int).tolist()).issubset({0, 1})

    def test_normalise_scalar(self, small_df: pl.DataFrame) -> None:
        scalar = compute_scalar_features(small_df)
        normed, mean, std = normalise_scalar(scalar)
        assert normed.shape == scalar.shape
        assert np.isfinite(normed).all()
        # After normalisation, mean should be near 0
        assert abs(normed.mean(axis=0).mean()) < 0.5

    def test_labels_adverse_selection_fill_reversion_model(self) -> None:
        n = 25
        ts = (np.arange(n) * 1_000_000).astype(np.int64)
        mid = np.ones(n, dtype=np.float64) * 100.0
        mid[1] = 100.2
        mid[2:8] = np.linspace(100.1, 99.7, 6)

        data: dict[str, np.ndarray] = {"timestamp_ns": ts}
        for lvl in range(1, 6):
            data[f"bid_price_{lvl}"] = mid - 0.01 * lvl
            data[f"ask_price_{lvl}"] = mid + 0.01 * lvl
            data[f"bid_size_{lvl}"] = np.ones(n, dtype=np.float64) * (1.0 + lvl)
            data[f"ask_size_{lvl}"] = np.ones(n, dtype=np.float64) * (1.0 + lvl)
        df = pl.DataFrame(data)

        labels = compute_labels(df, horizons=(1, 10, 12, 20))
        assert labels[0, 5] == pytest.approx(1.0)

    def test_trade_flow_features_are_finite_and_deterministic(self) -> None:
        rows = [
            {
                "timestamp_ns": 1,
                "best_bid": 100.0,
                "best_ask": 100.2,
                "last_trade_price": 100.1,
                "last_trade_size": 0.0,
                "recent_traded_volume": 0.0,
                "trade_direction": 255,
                **{f"bid_price_{i}": 100.0 - i * 0.01 for i in range(1, 6)},
                **{f"ask_price_{i}": 100.2 + i * 0.01 for i in range(1, 6)},
                **{f"bid_size_{i}": 0.0 for i in range(1, 6)},
                **{f"ask_size_{i}": 0.0 for i in range(1, 6)},
            },
            {
                "timestamp_ns": 2,
                "best_bid": 100.1,
                "best_ask": 100.3,
                "last_trade_price": 0.0,
                "last_trade_size": -2.0,
                "recent_traded_volume": -4.0,
                "trade_direction": 0,
                **{f"bid_price_{i}": 100.1 - i * 0.01 for i in range(1, 6)},
                **{f"ask_price_{i}": 100.3 + i * 0.01 for i in range(1, 6)},
                **{f"bid_size_{i}": 1.0 for i in range(1, 6)},
                **{f"ask_size_{i}": 1.0 for i in range(1, 6)},
            },
        ]
        df = pl.DataFrame(rows)
        scalar_first = compute_scalar_features(df)
        scalar_second = compute_scalar_features(df)
        assert np.isfinite(scalar_first).all()
        assert np.array_equal(scalar_first, scalar_second)
        assert scalar_first[1, TRADE_FLOW_FEATURE_INDICES["trade_signed_flow"]] == pytest.approx(-2.0)
        assert scalar_first[1, TRADE_FLOW_FEATURE_INDICES["trade_intensity_5"]] == pytest.approx(0.0)
        assert scalar_first[1, TRADE_FLOW_FEATURE_INDICES["trade_vs_mid_bps"]] == pytest.approx(0.0)

    def test_trade_flow_feature_schema_stable_under_column_ordering(self) -> None:
        df = _make_lob_df(n_ticks=32, seed=7)
        reordered = df.select(list(reversed(df.columns)))
        left = compute_scalar_features(df)
        right = compute_scalar_features(reordered)
        assert np.allclose(left, right)

    def test_shadow_schema_backfills_trade_flow_columns(self) -> None:
        df = _make_lob_df(n_ticks=10, seed=11).drop(
            ["last_trade_price", "last_trade_size", "recent_traded_volume", "trade_direction"]
        )
        out = _ensure_trade_flow_schema(df)
        for column in ("last_trade_price", "last_trade_size", "recent_traded_volume", "trade_direction"):
            assert column in out.columns
        scalar = compute_scalar_features(out)
        assert scalar.shape[1] == D_SCALAR

    def test_order_count_zero_semantics_fall_back_to_size_only_features(self) -> None:
        base_df = _make_lob_df(n_ticks=24, seed=13)
        with_missing_counts = base_df.with_columns(
            [pl.lit(0).alias(f"bid_oc_{i}") for i in range(1, 11)]
            + [pl.lit(0).alias(f"ask_oc_{i}") for i in range(1, 11)]
        )
        left = compute_scalar_features(base_df)
        right = compute_scalar_features(with_missing_counts)
        assert np.allclose(left, right)

    def test_order_count_presence_reweights_depth_imbalance(self) -> None:
        rows = [
            {
                "timestamp_ns": 1,
                "best_bid": 100.0,
                "best_ask": 100.2,
                "last_trade_price": 100.1,
                "last_trade_size": 0.1,
                "recent_traded_volume": 0.3,
                "trade_direction": 1,
                **{f"bid_price_{i}": 100.0 - i * 0.01 for i in range(1, 11)},
                **{f"ask_price_{i}": 100.2 + i * 0.01 for i in range(1, 11)},
                **{f"bid_size_{i}": 1.0 for i in range(1, 11)},
                **{f"ask_size_{i}": 1.0 for i in range(1, 11)},
                **{f"bid_oc_{i}": 20 for i in range(1, 11)},
                **{f"ask_oc_{i}": 2 for i in range(1, 11)},
            }
        ]
        df = pl.DataFrame(rows)
        scalar = compute_scalar_features(df)
        obi_index = 3
        qi_start = 13
        assert scalar[0, obi_index] > 0.5
        assert scalar[0, qi_start] > 0.0


# ── Model ─────────────────────────────────────────────────────────────────────


class TestModel:
    def test_spatial_encoder_shape(self) -> None:
        enc = LOBSpatialEncoder(d_model=32, n_heads=2, n_layers=1, n_levels=5)
        x = torch.randn(2, 16, 5, 4)
        out = enc(x)
        assert out.shape == (2, 16, 32)

    def test_temporal_encoder_shape(self) -> None:
        enc = TemporalEncoder(d_in=42, d_model=64, n_heads=4, n_layers=2)
        x = torch.randn(2, 16, 42)
        out = enc(x)
        assert out.shape == (2, 16, 64)

    def test_full_model_output_shapes(self) -> None:
        model = CryptoAlphaNet(d_spatial=32, d_temporal=64, seq_len=16)
        lob = torch.randn(2, 16, 5, 4)
        scalar = torch.randn(2, 16, D_SCALAR)
        preds = model(lob, scalar)
        assert preds["returns"].shape == (2, 16, 4)
        assert preds["direction"].shape == (2, 16, 3)
        assert preds["risk"].shape == (2, 16)

    def test_model_gradients(self) -> None:
        model = CryptoAlphaNet(d_spatial=32, d_temporal=64, seq_len=16)
        criterion = MultiTaskLoss()
        lob = torch.randn(2, 16, 5, 4)
        scalar = torch.randn(2, 16, D_SCALAR)
        labels = torch.randn(2, 16, 6)
        labels[..., 4] = torch.randint(0, 3, (2, 16)).float()
        labels[..., 5] = torch.randint(0, 2, (2, 16)).float()

        preds = model(lob, scalar)
        loss, _ = criterion(preds, labels)
        loss.backward()

        for name, param in model.named_parameters():
            if param.requires_grad:
                assert param.grad is not None, f"No gradient for {name}"

    def test_risk_head_bounded(self) -> None:
        model = CryptoAlphaNet(d_spatial=32, d_temporal=64, seq_len=8)
        lob = torch.randn(1, 8, 5, 4)
        scalar = torch.randn(1, 8, D_SCALAR)
        preds = model(lob, scalar)
        risk = preds["risk"].detach().numpy()
        assert (risk >= 0.0).all() and (risk <= 1.0).all(), "Risk not in [0, 1]"

    def test_model_exposes_risk_logits_for_stable_training(self) -> None:
        model = CryptoAlphaNet(d_spatial=16, d_temporal=32, seq_len=8)
        lob = torch.randn(1, 8, 5, 4)
        scalar = torch.randn(1, 8, D_SCALAR)
        preds = model(lob, scalar)
        assert "risk_logits" in preds
        assert preds["risk_logits"].shape == preds["risk"].shape


# ── Loss ──────────────────────────────────────────────────────────────────────


class TestLoss:
    def test_loss_positive(self) -> None:
        model = CryptoAlphaNet(d_spatial=32, d_temporal=64, seq_len=8)
        criterion = MultiTaskLoss()
        lob = torch.randn(2, 8, 5, 4)
        scalar = torch.randn(2, 8, D_SCALAR)
        labels = torch.zeros(2, 8, 6)
        labels[..., 4] = torch.randint(0, 3, (2, 8)).float()
        preds = model(lob, scalar)
        loss, breakdown = criterion(preds, labels)
        assert loss.item() > 0
        assert "loss_return" in breakdown

    def test_loss_decreases(self) -> None:
        """Loss should decrease with a gradient step."""
        model = CryptoAlphaNet(d_spatial=32, d_temporal=64, seq_len=8)
        criterion = MultiTaskLoss()
        optimizer = torch.optim.Adam(model.parameters(), lr=0.01)
        lob = torch.randn(4, 8, 5, 4)
        scalar = torch.randn(4, 8, D_SCALAR)
        labels = torch.zeros(4, 8, 6)
        labels[..., 4] = torch.randint(0, 3, (4, 8)).float()

        losses = []
        for _ in range(3):
            preds = model(lob, scalar)
            loss, _ = criterion(preds, labels)
            optimizer.zero_grad()
            loss.backward()
            optimizer.step()
            losses.append(loss.item())

        assert losses[-1] <= losses[0] * 2, "Loss did not stabilise"


# ── Dataset ───────────────────────────────────────────────────────────────────


class TestDataset:
    def test_dataset_len(self, medium_df: pl.DataFrame) -> None:
        ds = LOBDataset(medium_df, DatasetConfig(seq_len=32, stride=1))
        # T - seq_len + 1 windows
        assert len(ds) == 500 - 32 + 1

    def test_dataset_item_shapes(self, medium_df: pl.DataFrame) -> None:
        ds = LOBDataset(medium_df, DatasetConfig(seq_len=32))
        sample = ds[0]
        assert sample["lob"].shape == (32, 5, 4)
        assert sample["scalar"].shape == (32, D_SCALAR)
        assert sample["labels"].shape == (32, 6)
        assert sample["mask"].shape == (32,)

    def test_rolling_normalise_handles_short_series(self) -> None:
        x = np.arange(24, dtype=np.float64).reshape(12, 2)
        out = rolling_normalise(x, window=20)
        assert out.shape == x.shape
        assert np.isfinite(out).all()

    def test_rolling_normalise_accepts_history_without_leakage(self) -> None:
        history = np.array([[0.0, 0.0], [1.0, 1.0]], dtype=np.float64)
        x = np.array([[2.0, 2.0], [3.0, 3.0]], dtype=np.float64)
        out = rolling_normalise(x, window=3, history=history)
        expected_first = (2.0 - 1.0) / np.sqrt(2.0 / 3.0)
        assert out.shape == x.shape
        assert out[0, 0] == pytest.approx(expected_first, rel=1e-5)
        assert np.isfinite(out).all()

    def test_walk_forward_no_leakage(self, medium_df: pl.DataFrame) -> None:
        splits = split_walk_forward(medium_df, n_folds=3, train_frac=0.75)
        for train_df, test_df in splits:
            # Test set must come after train set in time
            assert train_df["timestamp_ns"].max() <= test_df["timestamp_ns"].min()

    # ── Backtest ──────────────────────────────────────────────────────────────────

    def test_walk_forward_with_tiny_dataset_has_non_empty_train(self) -> None:
        tiny = _make_lob_df(n_ticks=40, seed=2)
        cfg = DatasetConfig(seq_len=8, horizons=(1, 5, 10, 20))
        splits = split_walk_forward(
            tiny,
            n_folds=2,
            train_frac=0.6,
        )
        assert len(splits) == 2
        for train_df, test_df in splits:
            train_ds = LOBDataset(train_df, cfg)
            test_ds = LOBDataset(test_df, cfg)
            assert len(train_ds) > 0
            assert len(test_ds) > 0

    def test_split_train_validation_is_chronological(self, medium_df: pl.DataFrame) -> None:
        fit_train_df, val_df = split_train_validation(medium_df, validation_frac=0.2, min_samples=32)
        assert len(fit_train_df) + len(val_df) == len(medium_df)
        assert len(val_df) > 0
        assert fit_train_df["timestamp_ns"].max() < val_df["timestamp_ns"].min()

    def test_split_train_validation_returns_empty_validation_when_fold_too_short(self) -> None:
        tiny = _make_lob_df(n_ticks=40, seed=3)
        fit_train_df, val_df = split_train_validation(tiny, validation_frac=0.5, min_samples=24)
        assert len(fit_train_df) == len(tiny)
        assert len(val_df) == 0

    def test_walk_forward_train_uses_internal_validation_for_selection(self) -> None:
        df = _make_lob_df(n_ticks=240, seed=4)
        cfg = TrainerConfig(
            seq_len=16,
            epochs=1,
            batch_size=8,
            n_folds=2,
            train_frac=0.75,
            validation_frac=0.2,
            log_every_epochs=1,
            use_amp=False,
        )
        results = walk_forward_train(df, cfg)
        assert results
        for result in results:
            assert "validation_metrics" in result
            assert result["selection_score"] == pytest.approx(
                result["best_selection_score"], rel=1e-6, abs=1e-6
            )

    def test_walk_forward_train_can_run_quietly(self, capsys: pytest.CaptureFixture[str]) -> None:
        df = _make_lob_df(n_ticks=240, seed=40)
        cfg = TrainerConfig(
            seq_len=16,
            epochs=1,
            batch_size=8,
            n_folds=2,
            train_frac=0.75,
            validation_frac=0.2,
            log_every_epochs=1,
            use_amp=False,
            verbose=False,
        )

        results = walk_forward_train(df, cfg)

        captured = capsys.readouterr()
        assert results
        assert captured.out == ""

    def test_model_raises_on_invalid_lob_tensor_shape(self) -> None:
        model = CryptoAlphaNet(d_spatial=16, d_temporal=32, seq_len=8)
        bad_lob = torch.randn(2, 8, 5)
        scalar = torch.randn(2, 8, D_SCALAR)
        with pytest.raises(ValueError):
            model(bad_lob, scalar)

    def test_nan_inputs_propagate_to_outputs(self) -> None:
        model = CryptoAlphaNet(d_spatial=16, d_temporal=32, seq_len=8)
        lob = torch.randn(1, 8, 5, 4)
        scalar = torch.randn(1, 8, D_SCALAR)
        scalar[0, 0, 0] = float("nan")
        out = model(lob, scalar)
        assert torch.isnan(out["returns"]).any() or torch.isnan(out["risk"]).any()


class TestBacktest:
    def test_no_trades_with_zero_signal(self, small_df: pl.DataFrame) -> None:
        bt = NeuralAlphaBacktest()
        sig = np.zeros(len(small_df))
        res = bt.run(small_df, sig)
        assert res["trades"].is_empty() or len(res["trades"]) == 0

    def test_trades_with_strong_signal(self, small_df: pl.DataFrame) -> None:
        bt = NeuralAlphaBacktest(BacktestConfig(entry_threshold_bps=0.1))
        sig = np.ones(len(small_df)) * 0.01  # strong long signal
        res = bt.run(small_df, sig)
        assert res["metrics"]["total_trades"] > 0

    def test_equity_length(self, small_df: pl.DataFrame) -> None:
        bt = NeuralAlphaBacktest()
        sig = np.random.randn(len(small_df)) * 0.001
        res = bt.run(small_df, sig)
        if not res["equity_curve"].is_empty():
            assert len(res["equity_curve"]) > 0

    def test_metrics_keys(self, small_df: pl.DataFrame) -> None:
        bt = NeuralAlphaBacktest(BacktestConfig(entry_threshold_bps=0.1))
        sig = np.ones(len(small_df)) * 0.01
        res = bt.run(small_df, sig)
        for key in ("total_trades", "total_net_pnl", "sharpe_annualised", "win_rate"):
            assert key in res["metrics"]

    def test_market_impact_moves_price_by_side(self) -> None:
        bt = NeuralAlphaBacktest(BacktestConfig(impact_coeff=0.5, adv_usd=10_000, random_seed=1))
        buy_px = bt._apply_market_impact(100.0, 0.5)
        sell_px = bt._apply_market_impact(100.0, -0.5)
        assert buy_px > 100.0
        assert sell_px < 100.0

    def test_queue_fill_probability_bounded(self) -> None:
        bt = NeuralAlphaBacktest(BacktestConfig(queue_min_fill_prob=0.05, queue_max_fill_prob=0.9))
        p = bt._queue_fill_probability(level_price=100.0, qty=0.001)
        assert 0.05 <= p <= 0.9

    def test_time_aware_sharpe_is_finite(self, small_df: pl.DataFrame) -> None:
        ts = np.array(small_df["timestamp_ns"].to_list(), dtype=np.int64)
        ts[1:] += np.arange(1, len(ts)) * 137
        df = small_df.with_columns(pl.Series("timestamp_ns", ts))
        cfg = BacktestConfig(entry_threshold_bps=0.1, random_seed=7)
        bt = NeuralAlphaBacktest(cfg)
        sig = np.ones(len(df), dtype=np.float64) * 0.01
        res = bt.run(df, sig)
        assert np.isfinite(res["metrics"]["sharpe_annualised"])


# ── Alpha regression ──────────────────────────────────────────────────────────


class TestAlphaRegression:
    def test_ic_perfect_signal(self) -> None:
        r = np.linspace(-1, 1, 100)
        ic_mean, ic_std, icir = compute_ic(r, r, rolling_window=10)
        assert ic_mean > 0.9, f"Expected IC near 1.0, got {ic_mean}"

    def test_ic_anti_correlated(self) -> None:
        r = np.linspace(-1, 1, 100)
        ic_mean, _, _ = compute_ic(-r, r, rolling_window=10)
        assert ic_mean < -0.9

    def test_hit_rate_perfect(self) -> None:
        r = np.random.randn(100)
        hit = compute_hit_rate(r, r)
        assert hit == 1.0

    def test_hit_rate_random(self) -> None:
        rng = np.random.default_rng(0)
        s = rng.standard_normal(1000)
        r = rng.standard_normal(1000)
        hit = compute_hit_rate(s, r)
        # Random signals should be around 50% hit rate
        assert 0.3 < hit < 0.7

    def test_ols_regression_perfect(self) -> None:
        s = np.linspace(0, 1, 100)
        r = 0.5 + 2.0 * s + np.random.randn(100) * 0.01
        alpha, beta, t, p, r2 = ols_regression(s, r)
        assert abs(beta - 2.0) < 0.1
        assert r2 > 0.9

    def test_turnover(self) -> None:
        s = np.array([0.0, 1.0, 0.0, 1.0, 0.0])
        assert compute_turnover(s) == pytest.approx(1.0)

    def test_analyse_alpha_no_crash(self) -> None:
        rng = np.random.default_rng(0)
        fold_results = [
            {
                "fold": 1,
                "predictions": rng.standard_normal((50, 4)).astype(np.float32),
                "labels": rng.standard_normal((50, 6)).astype(np.float32),
            }
        ]
        metrics = analyse_alpha(fold_results)
        assert metrics.n_samples == 50

    def test_expected_calibration_error_matches_simple_case(self) -> None:
        confidences = np.array([0.9, 0.8, 0.4, 0.3], dtype=np.float32)
        outcomes = np.array([1.0, 1.0, 0.0, 0.0], dtype=np.float32)
        ece, bins = expected_calibration_error(confidences, outcomes, n_bins=2)
        assert ece == pytest.approx(0.25)
        assert len(bins) == 2

    def test_direction_calibration_uses_sign_aligned_probabilities(self) -> None:
        fold_results = [
            {
                "fold": 1,
                "predictions": np.array(
                    [[0.0, 0.0, 0.8, 0.0], [0.0, 0.0, -0.7, 0.0], [0.0, 0.0, 0.0, 0.0]],
                    dtype=np.float32,
                ),
                "direction_probs": np.array(
                    [[0.1, 0.2, 0.7], [0.6, 0.1, 0.3], [0.2, 0.6, 0.2]],
                    dtype=np.float32,
                ),
                "labels": np.array(
                    [[0.0, 0.0, 0.5, 0.0, 2.0, 0.0], [0.0, 0.0, -0.4, 0.0, 0.0, 0.0], [0.0, 0.0, 0.0, 0.0, 1.0, 0.0]],
                    dtype=np.float32,
                ),
            }
        ]
        report = analyse_direction_calibration(fold_results, horizon_idx=2, n_bins=4)
        assert report.mean_signed_precision == pytest.approx(1.0)
        assert report.mean_confidence == pytest.approx(0.65)
        assert report.coverage == pytest.approx(2.0 / 3.0)

    def test_threshold_sweep_reduces_coverage_as_floor_rises(self) -> None:
        fold_results = [
            {
                "fold": 1,
                "predictions": np.array(
                    [[0.0, 0.0, 0.8, 0.0], [0.0, 0.0, -0.7, 0.0], [0.0, 0.0, 0.6, 0.0]],
                    dtype=np.float32,
                ),
                "direction_probs": np.array(
                    [[0.1, 0.2, 0.7], [0.45, 0.3, 0.25], [0.2, 0.3, 0.55]],
                    dtype=np.float32,
                ),
                "labels": np.array(
                    [[0.0, 0.0, 0.5, 0.0, 2.0, 0.0], [0.0, 0.0, -0.4, 0.0, 0.0, 0.0], [0.0, 0.0, 0.2, 0.0, 2.0, 0.0]],
                    dtype=np.float32,
                ),
            }
        ]
        sweep = sweep_direction_thresholds(fold_results, thresholds=[0.4, 0.6], horizon_idx=2)
        assert sweep[0].coverage > sweep[1].coverage
        assert sweep[0].hit_rate >= sweep[1].hit_rate

    def test_flat_threshold_sensitivity_expands_flat_share(self) -> None:
        fold_results = [
            {
                "fold": 1,
                "predictions": np.zeros((4, 4), dtype=np.float32),
                "labels": np.array(
                    [
                        [0.0, 0.0, -0.00003, 0.0, 0.0, 0.0],
                        [0.0, 0.0, -0.00001, 0.0, 1.0, 0.0],
                        [0.0, 0.0, 0.00001, 0.0, 1.0, 0.0],
                        [0.0, 0.0, 0.00004, 0.0, 2.0, 0.0],
                    ],
                    dtype=np.float32,
                ),
            }
        ]
        points = analyse_flat_threshold_sensitivity(fold_results, thresholds_bps=[0.1, 0.3], horizon_idx=2)
        assert points[1].flat_share > points[0].flat_share


class TestShadowSessionTraining:
    def test_symbol_model_path_uses_symbol_specific_name(self) -> None:
        assert _symbol_model_path("BTCUSDT") == Path("models/neural_alpha_btcusdt_latest.pt")
        assert _symbol_model_path("ETHUSDT", "secondary") == Path(
            "models/neural_alpha_ethusdt_secondary.pt"
        )

    def test_train_on_recent_persists_model_weights(
        self, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        model_path = tmp_path / "neural_alpha_latest.pt"
        signal_path = tmp_path / "alpha_signal.bin"
        log_path = tmp_path / "shadow.jsonl"

        cfg = ShadowSessionConfig(
            model_path=str(model_path),
            log_path=str(log_path),
            signal_file=str(signal_path),
            exchanges=["BINANCE"],
            train_epochs=1,
            seq_len=8,
            d_spatial=16,
            d_temporal=32,
        )
        session = NeuralAlphaShadowSession(cfg)

        tick = {
            "timestamp_ns": 1,
            "exchange": "BINANCE",
            "best_bid": 100.0,
            "best_ask": 100.1,
            "bid_price_1": 100.0,
            "ask_price_1": 100.1,
            "bid_size_1": 1.0,
            "ask_size_1": 1.0,
            "bid_price_2": 99.9,
            "ask_price_2": 100.2,
            "bid_size_2": 1.0,
            "ask_size_2": 1.0,
            "bid_price_3": 99.8,
            "ask_price_3": 100.3,
            "bid_size_3": 1.0,
            "ask_size_3": 1.0,
            "bid_price_4": 99.7,
            "ask_price_4": 100.4,
            "bid_size_4": 1.0,
            "ask_size_4": 1.0,
            "bid_price_5": 99.6,
            "ask_price_5": 100.5,
            "bid_size_5": 1.0,
            "ask_size_5": 1.0,
        }

        def _fake_fetch(symbol: str) -> dict:
            nonlocal tick
            tick = {**tick, "timestamp_ns": tick["timestamp_ns"] + 1}
            return tick

        def _fake_walk_forward_train(df: pl.DataFrame, cfg_local: object) -> list[dict]:
            model = CryptoAlphaNet(d_spatial=16, d_temporal=32, seq_len=8)
            return [{"metrics": {"loss_total": 0.1234}, "model_state": model.state_dict()}]

        monkeypatch.setattr(
            "research.neural_alpha.runtime.shadow_session._fetch_binance_l5", _fake_fetch
        )
        monkeypatch.setattr(
            "research.neural_alpha.runtime.shadow_session.walk_forward_train",
            _fake_walk_forward_train,
        )

        session.train_on_recent(n_ticks=4)

        assert model_path.exists()
        assert model_path.with_suffix(".json").exists()

        state = torch.load(model_path, map_location="cpu", weights_only=True)
        assert isinstance(state, dict)
        assert any(k.startswith("spatial_enc.") for k in state.keys())

        session._log_fp.close()
        session._publisher.close()

    def test_main_loads_secondary_model_after_train_on_recent(
        self, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        import sys
        from research.neural_alpha.runtime.shadow_session import main
        from research.neural_alpha.models.model import CryptoAlphaNet

        primary_path = tmp_path / "neural_alpha_btcusdt_latest.pt"
        secondary_path = tmp_path / "neural_alpha_btcusdt_secondary.pt"
        log_path = tmp_path / "shadow.jsonl"
        signal_path = tmp_path / "alpha_signal.bin"

        secondary_model = CryptoAlphaNet(d_spatial=32, d_temporal=64, n_temp_layers=1, seq_len=8)
        torch.save(secondary_model.state_dict(), secondary_path)

        monkeypatch.setattr(
            sys,
            "argv",
            [
                "shadow_session",
                "--train-ticks",
                "4",
                "--train-epochs",
                "1",
                "--seq-len",
                "8",
                "--d-spatial",
                "16",
                "--d-temporal",
                "32",
                "--model-path",
                str(primary_path),
                "--secondary-model-path",
                str(secondary_path),
                "--log-path",
                str(log_path),
                "--signal-file",
                str(signal_path),
                "--no-require-full-model-stack",
                "--duration",
                "0",
            ],
        )

        tick_base = {
            "timestamp_ns": 1,
            "exchange": "BINANCE",
            "best_bid": 100.0,
            "best_ask": 100.1,
            "bid_price_1": 100.0,
            "ask_price_1": 100.1,
            "bid_size_1": 1.0,
            "ask_size_1": 1.0,
            "bid_price_2": 99.9,
            "ask_price_2": 100.2,
            "bid_size_2": 1.0,
            "ask_size_2": 1.0,
            "bid_price_3": 99.8,
            "ask_price_3": 100.3,
            "bid_size_3": 1.0,
            "ask_size_3": 1.0,
            "bid_price_4": 99.7,
            "ask_price_4": 100.4,
            "bid_size_4": 1.0,
            "ask_size_4": 1.0,
            "bid_price_5": 99.6,
            "ask_price_5": 100.5,
            "bid_size_5": 1.0,
            "ask_size_5": 1.0,
        }
        counter = {"t": 0}

        def _fake_fetch(symbol: str) -> dict:
            counter["t"] += 1
            return {**tick_base, "timestamp_ns": counter["t"]}

        def _fake_walk_forward_train(df: pl.DataFrame, cfg_local: object) -> list[dict]:
            model = CryptoAlphaNet(d_spatial=16, d_temporal=32, seq_len=8)
            return [{"metrics": {"loss_total": 0.5}, "model_state": model.state_dict()}]

        loaded_secondary: list[bool] = []

        def _fake_load_secondary(self_session, path: str) -> None:
            loaded_secondary.append(True)
            m = CryptoAlphaNet(d_spatial=32, d_temporal=64, n_temp_layers=1, seq_len=8)
            state = torch.load(path, map_location="cpu", weights_only=True)
            m.load_state_dict(state)
            self_session._secondary_model = m

        monkeypatch.setattr(
            "research.neural_alpha.runtime.shadow_session._fetch_binance_l5", _fake_fetch
        )
        monkeypatch.setattr(
            "research.neural_alpha.runtime.shadow_session.walk_forward_train",
            _fake_walk_forward_train,
        )
        monkeypatch.setattr(
            "research.neural_alpha.runtime.shadow_session.NeuralAlphaShadowSession.load_secondary_model",
            _fake_load_secondary,
        )
        monkeypatch.setattr(
            "research.neural_alpha.runtime.shadow_session.NeuralAlphaShadowSession.run",
            lambda self_session: None,
        )

        main()

        assert loaded_secondary, "load_secondary_model was not called after train_on_recent"




class TestShadowRetrainCadence:
    def test_continuous_retrain_respects_minimum_wall_clock_interval(
        self, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        cfg = ShadowSessionConfig(
            log_path=str(tmp_path / "shadow.jsonl"),
            signal_file=str(tmp_path / "alpha_signal.bin"),
            exchanges=["BINANCE"],
            continuous_train_every_ticks=1,
            min_continuous_train_interval_s=600,
            seq_len=8,
        )
        session = NeuralAlphaShadowSession(cfg)
        calls: list[int] = []
        monkeypatch.setattr(session, "train_on_recent", lambda n_ticks: calls.append(n_ticks))
        session._processed_ticks = 1
        session._last_continuous_train_tick = 0
        session._session_steady_start_ns = 0
        session._last_continuous_train_steady_ns = 0
        monkeypatch.setattr(
            "research.neural_alpha.runtime.shadow_session.time.monotonic_ns",
            lambda: 120 * 1_000_000_000,
        )

        session._maybe_continuous_train()

        assert calls == []
        monkeypatch.setattr(
            "research.neural_alpha.runtime.shadow_session.time.monotonic_ns",
            lambda: 700 * 1_000_000_000,
        )

        session._maybe_continuous_train()

        assert len(calls) == 1
        session._log_fp.close()
        session._publisher.close()
        session._regime_publisher.close()

    def test_regime_retrain_respects_startup_warmup_and_minimum_interval(
        self, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        cfg = ShadowSessionConfig(
            log_path=str(tmp_path / "shadow.jsonl"),
            signal_file=str(tmp_path / "alpha_signal.bin"),
            exchanges=["BINANCE"],
            regime_retrain_every_ticks=1,
            min_regime_train_interval_s=900,
            regime_startup_warmup_s=300,
            seq_len=8,
        )
        session = NeuralAlphaShadowSession(cfg)
        session._processed_ticks = 1
        session._last_regime_train_tick = 0
        session._session_steady_start_ns = 0
        session._last_regime_train_steady_ns = 0
        monkeypatch.setattr(
            session,
            "_collect_training_ticks",
            lambda n_ticks: pl.DataFrame(
                [
                    {
                        "timestamp_ns": 1,
                        "best_bid": 100.0,
                        "best_ask": 100.1,
                        "bid_size_1": 1.0,
                        "ask_size_1": 1.0,
                    }
                ]
            ),
        )
        calls: list[int] = []
        monkeypatch.setattr(session, "_train_regime_on_data", lambda df: calls.append(len(df)))
        monkeypatch.setattr(
            "research.neural_alpha.runtime.shadow_session.time.monotonic_ns",
            lambda: 200 * 1_000_000_000,
        )

        session._maybe_regime_retrain()

        assert calls == []
        monkeypatch.setattr(
            "research.neural_alpha.runtime.shadow_session.time.monotonic_ns",
            lambda: 600 * 1_000_000_000,
        )

        session._maybe_regime_retrain()

        assert calls == []
        monkeypatch.setattr(
            "research.neural_alpha.runtime.shadow_session.time.monotonic_ns",
            lambda: 901 * 1_000_000_000,
        )

        session._maybe_regime_retrain()

        assert len(calls) == 1
        session._log_fp.close()
        session._publisher.close()
        session._regime_publisher.close()


class TestShadowRetrainDiagnostics:
    """Verify that retrain failures emit full diagnostics and are counted by error class."""

    def _make_session(self, tmp_path: Path) -> NeuralAlphaShadowSession:
        cfg = ShadowSessionConfig(
            log_path=str(tmp_path / "shadow.jsonl"),
            signal_file=str(tmp_path / "alpha_signal.bin"),
            exchanges=["BINANCE"],
            continuous_train_every_ticks=1,
            min_continuous_train_interval_s=0,
            regime_retrain_every_ticks=1,
            min_regime_train_interval_s=0,
            regime_startup_warmup_s=0,
            seq_len=8,
        )
        session = NeuralAlphaShadowSession(cfg)
        session._processed_ticks = 1
        session._last_continuous_train_tick = 0
        session._last_regime_train_tick = 0
        session._session_steady_start_ns = 0
        session._last_continuous_train_steady_ns = 0
        session._last_regime_train_steady_ns = 0
        return session

    def test_continuous_retrain_failure_logs_exc_class_and_traceback(
        self, tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture
    ) -> None:
        session = self._make_session(tmp_path)

        def _boom(n_ticks: int) -> None:
            raise ValueError("injected type mismatch")

        monkeypatch.setattr(session, "train_on_recent", _boom)
        monkeypatch.setattr(
            "research.neural_alpha.runtime.shadow_session.time.monotonic_ns",
            lambda: 700 * 1_000_000_000,
        )

        session._maybe_continuous_train()

        captured = capsys.readouterr().out
        assert "exc_class=ValueError" in captured
        assert "injected type mismatch" in captured
        assert "Traceback" in captured
        assert "context=" in captured
        assert session._retrain_failure_counts == {"ValueError": 1}

        session._log_fp.close()
        session._publisher.close()
        session._regime_publisher.close()

    def test_regime_retrain_failure_logs_exc_class_and_traceback(
        self, tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture
    ) -> None:
        session = self._make_session(tmp_path)

        def _boom_collect(n_ticks: int) -> None:
            raise RuntimeError("bad data")

        monkeypatch.setattr(session, "_collect_training_ticks", _boom_collect)
        monkeypatch.setattr(
            "research.neural_alpha.runtime.shadow_session.time.monotonic_ns",
            lambda: 1200 * 1_000_000_000,
        )

        session._maybe_regime_retrain()

        captured = capsys.readouterr().out
        assert "exc_class=RuntimeError" in captured
        assert "bad data" in captured
        assert "Traceback" in captured
        assert session._regime_retrain_failure_counts == {"RuntimeError": 1}

        session._log_fp.close()
        session._publisher.close()
        session._regime_publisher.close()

    def test_summary_includes_per_error_class_failure_counts(
        self, tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture
    ) -> None:
        session = self._make_session(tmp_path)
        session._retrain_failure_counts = {"ValueError": 3, "RuntimeError": 1}
        session._regime_retrain_failure_counts = {"KeyError": 2}

        # Minimal signal records so _print_summary doesn't short-circuit
        session._signal_records = [
            {
                "event_index": 1,
                "session_elapsed_ns": 0,
                "mid_price": 100.0,
                "signal": 0.1,
                "ret_mid_bps": 0.5,
                "p_calm": 1.0,
                "p_trending": 0.0,
                "p_shock": 0.0,
                "p_illiquid": 0.0,
                "exchange": "BINANCE",
                "timestamp_ns": 1,
                "timestamp_exchange_ns": 1,
                "timestamp_local_ns": 1,
            }
        ]

        session._print_summary()

        import json as _json
        captured = capsys.readouterr().out
        summary_line = next(l for l in captured.splitlines() if "[ShadowSummary]" in l)
        payload = _json.loads(summary_line.split("[ShadowSummary] ", 1)[1])
        assert payload["retrain_failure_counts"] == {"ValueError": 3, "RuntimeError": 1}
        assert payload["regime_retrain_failure_counts"] == {"KeyError": 2}

        session._log_fp.close()
        session._publisher.close()
        session._regime_publisher.close()


class TestShadowTimestampMetrics:
    def test_regime_churn_summary_reports_switch_rate_and_confidence(self) -> None:
        metrics = _summarise_regime_churn(
            [
                {"event_index": 1, "session_elapsed_ns": 0, "p_calm": 0.9, "p_trending": 0.1, "p_shock": 0.0, "p_illiquid": 0.0},
                {"event_index": 2, "session_elapsed_ns": 30_000_000_000, "p_calm": 0.2, "p_trending": 0.7, "p_shock": 0.1, "p_illiquid": 0.0},
                {"event_index": 3, "session_elapsed_ns": 60_000_000_000, "p_calm": 0.85, "p_trending": 0.1, "p_shock": 0.05, "p_illiquid": 0.0},
            ]
        )

        assert metrics["dominant_regime"] == "calm"
        assert metrics["dominant_regime_change_count"] == 2
        assert metrics["switches_per_minute"] == pytest.approx(2.0)
        assert metrics["average_dominant_confidence"] == pytest.approx((0.9 + 0.7 + 0.85) / 3.0)

    def test_signal_alignment_uses_event_order(self) -> None:
        records = [
            {"event_index": 2, "session_elapsed_ns": 2, "mid_price": 101.0, "signal": 0.2},
            {"event_index": 1, "session_elapsed_ns": 1, "mid_price": 100.0, "signal": 0.1},
            {"event_index": 3, "session_elapsed_ns": 3, "mid_price": 102.0, "signal": 0.3},
        ]

        (signals, outcomes) = _build_signal_alignment(records)

        assert signals.tolist() == pytest.approx([0.1, 0.2])
        assert outcomes.tolist() == pytest.approx([0.01, 1.0 / 101.0], rel=1e-6)

    def test_timestamp_quality_flags_non_monotonic_records(self) -> None:
        records = [
            {
                "event_index": 1,
                "session_elapsed_ns": 1,
                "timestamp_exchange_ns": 10,
                "timestamp_local_ns": 100,
                "exchange": "BINANCE",
            },
            {
                "event_index": 2,
                "session_elapsed_ns": 2,
                "timestamp_exchange_ns": 9,
                "timestamp_local_ns": 99,
                "exchange": "BINANCE",
            },
        ]

        diagnostics = _summarise_timestamp_quality(records)

        assert diagnostics["has_timestamp_issues"] is True
        assert diagnostics["exchange_non_monotonic"] == 1
        assert diagnostics["local_non_monotonic"] == 1

    def test_timestamp_quality_allows_large_cross_venue_clock_offsets(self) -> None:
        records = [
            {
                "event_index": 1,
                "session_elapsed_ns": 1,
                "timestamp_exchange_ns": 10,
                "timestamp_local_ns": 100,
                "exchange": "BINANCE",
            },
            {
                "event_index": 2,
                "session_elapsed_ns": 2,
                "timestamp_exchange_ns": 10 + 120 * 1_000_000_000,
                "timestamp_local_ns": 101,
                "exchange": "OKX",
            },
        ]

        diagnostics = _summarise_timestamp_quality(records)

        assert diagnostics["has_timestamp_issues"] is False


    def test_analyse_ops_events_uses_shadow_health_summary(self) -> None:
        ops = analyse_ops_events([
            {
                "event": "shadow_health_summary",
                "data_quality": {
                    "per_venue": {
                        "BINANCE": {
                            "ticks_received": 10,
                            "ticks_used": 8,
                            "missing_venue_incidents": 2,
                            "rest_fallback_usage": 1,
                            "resnapshot_count": 1,
                        }
                    }
                },
                "model_quality": {"gating_reason_counts": {"confidence_gate": 2}},
            },
            {"event": "safe_mode_activated", "reason": "drift"},
            {"event": "continuous_retrain_completed"},
        ])

        assert ops["safe_mode_activations"] == 1
        assert ops["continuous_retrain_completions"] == 1
        assert ops["health"]["data_quality"]["per_venue"]["BINANCE"]["ticks_received"] == 10

    def test_fetch_tick_records_rest_fallback_without_ops_startup_counters(
        self, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        signal_path = tmp_path / "alpha_signal.bin"
        log_path = tmp_path / "shadow.jsonl"
        cfg = ShadowSessionConfig(
            log_path=str(log_path),
            signal_file=str(signal_path),
            exchanges=["BINANCE"],
            seq_len=8,
        )
        session = NeuralAlphaShadowSession(cfg)

        monkeypatch.setattr(session._bridge, "read_new_ticks", lambda: [])
        monkeypatch.setattr(
            "research.neural_alpha.runtime.shadow_session._fetch_binance_l5",
            lambda symbol: {
                "timestamp_ns": 1,
                "exchange": "BINANCE",
                "best_bid": 100.0,
                "best_ask": 100.1,
                "bid_price_1": 100.0,
                "ask_price_1": 100.1,
                "bid_size_1": 1.0,
                "ask_size_1": 1.0,
                "bid_price_2": 99.9,
                "ask_price_2": 100.2,
                "bid_size_2": 1.0,
                "ask_size_2": 1.0,
                "bid_price_3": 99.8,
                "ask_price_3": 100.3,
                "bid_size_3": 1.0,
                "ask_size_3": 1.0,
                "bid_price_4": 99.7,
                "ask_price_4": 100.4,
                "bid_size_4": 1.0,
                "ask_size_4": 1.0,
                "bid_price_5": 99.6,
                "ask_price_5": 100.5,
                "bid_size_5": 1.0,
                "ask_size_5": 1.0,
            },
        )

        ticks = session._fetch_tick()

        assert len(ticks) == 1
        stats = session._venue_stats["BINANCE"]
        assert stats.missing_venue_incidents == 1
        assert stats.rest_fallback_usage >= 1
        assert not stats.startup_confirmed
        assert stats.last_source == "rest_fallback"
        assert ticks[0]["tick_source"] == "rest_fallback"

        ticks = session._fetch_tick()

        stats = session._venue_stats["BINANCE"]
        assert len(ticks) == 1
        assert stats.missing_venue_incidents == 2
        assert stats.resnapshot_count == 0
        assert not stats.startup_confirmed

        session._log_fp.close()
        session._publisher.close()
        session._regime_publisher.close()

    def test_shadow_metrics_uses_session_elapsed_for_duration_and_blocks_bad_timestamps(self) -> None:
        rows = [
            {
                "event_index": 1,
                "session_elapsed_ns": 0,
                "timestamp_ns": 1_000,
                "timestamp_exchange_ns": 1_000,
                "timestamp_local_ns": 1_000,
                "mid_price": 100.0,
                "signal": 0.1,
                "risk_score": 0.2,
                "exchange": "BINANCE",
            },
            {
                "event_index": 2,
                "session_elapsed_ns": 30_000_000_000,
                "timestamp_ns": 999,
                "timestamp_exchange_ns": 999,
                "timestamp_local_ns": 999,
                "mid_price": 101.0,
                "signal": 0.2,
                "risk_score": 0.3,
                "exchange": "BINANCE",
            },
        ]

        metrics = analyse_signals(rows)

        assert metrics["duration_min"] == pytest.approx(0.5)
        assert metrics["timestamp_quality"]["has_timestamp_issues"] is True
        assert metrics["ic"] == 0.0
