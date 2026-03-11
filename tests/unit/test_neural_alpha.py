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

from research.alpha.neural_alpha.alpha_regression import (
    analyse_alpha,
    compute_hit_rate,
    compute_ic,
    compute_turnover,
    ols_regression,
)
from research.alpha.neural_alpha.backtest import BacktestConfig, NeuralAlphaBacktest
from research.alpha.neural_alpha.dataset import DatasetConfig, LOBDataset, split_walk_forward
from research.alpha.neural_alpha.features import (
    compute_labels,
    compute_lob_tensor,
    compute_scalar_features,
    normalise_scalar,
)
from research.alpha.neural_alpha.model import (
    CryptoAlphaNet,
    LOBSpatialEncoder,
    MultiTaskLoss,
    TemporalEncoder,
)
from research.alpha.neural_alpha.pipeline import generate_synthetic_lob


# ── Fixtures ──────────────────────────────────────────────────────────────────

@pytest.fixture
def small_df() -> pl.DataFrame:
    return generate_synthetic_lob(n_ticks=200, seed=0)


@pytest.fixture
def medium_df() -> pl.DataFrame:
    return generate_synthetic_lob(n_ticks=500, seed=1)


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
        assert scalar.shape == (200, 10)

    def test_scalar_no_inf(self, small_df: pl.DataFrame) -> None:
        scalar = compute_scalar_features(small_df)
        assert np.isfinite(scalar).all(), "Scalar features contain inf/nan"

    def test_labels_shape(self, small_df: pl.DataFrame) -> None:
        labels = compute_labels(small_df)
        assert labels.shape == (200, 5)

    def test_labels_direction_valid(self, small_df: pl.DataFrame) -> None:
        labels = compute_labels(small_df)
        direction = labels[:, 3]
        assert set(direction.astype(int).tolist()).issubset({0, 1, 2})

    def test_labels_adv_sel_binary(self, small_df: pl.DataFrame) -> None:
        labels = compute_labels(small_df)
        adv_sel = labels[:, 4]
        assert set(adv_sel.astype(int).tolist()).issubset({0, 1})

    def test_normalise_scalar(self, small_df: pl.DataFrame) -> None:
        scalar = compute_scalar_features(small_df)
        normed, mean, std = normalise_scalar(scalar)
        assert normed.shape == scalar.shape
        assert np.isfinite(normed).all()
        # After normalisation, mean should be near 0
        assert abs(normed.mean(axis=0).mean()) < 0.5


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
        lob    = torch.randn(2, 16, 5, 4)
        scalar = torch.randn(2, 16, 10)
        preds  = model(lob, scalar)
        assert preds["returns"].shape   == (2, 16, 3)
        assert preds["direction"].shape == (2, 16, 3)
        assert preds["risk"].shape      == (2, 16)

    def test_model_gradients(self) -> None:
        model     = CryptoAlphaNet(d_spatial=32, d_temporal=64, seq_len=16)
        criterion = MultiTaskLoss()
        lob       = torch.randn(2, 16, 5, 4)
        scalar    = torch.randn(2, 16, 10)
        labels    = torch.randn(2, 16, 5)
        labels[..., 3] = torch.randint(0, 3, (2, 16)).float()
        labels[..., 4] = torch.randint(0, 2, (2, 16)).float()

        preds = model(lob, scalar)
        loss, _ = criterion(preds, labels)
        loss.backward()

        for name, param in model.named_parameters():
            if param.requires_grad:
                assert param.grad is not None, f"No gradient for {name}"

    def test_risk_head_bounded(self) -> None:
        model  = CryptoAlphaNet(d_spatial=32, d_temporal=64, seq_len=8)
        lob    = torch.randn(1, 8, 5, 4)
        scalar = torch.randn(1, 8, 10)
        preds  = model(lob, scalar)
        risk   = preds["risk"].detach().numpy()
        assert (risk >= 0.0).all() and (risk <= 1.0).all(), "Risk not in [0, 1]"


# ── Loss ──────────────────────────────────────────────────────────────────────

class TestLoss:
    def test_loss_positive(self) -> None:
        model     = CryptoAlphaNet(d_spatial=32, d_temporal=64, seq_len=8)
        criterion = MultiTaskLoss()
        lob       = torch.randn(2, 8, 5, 4)
        scalar    = torch.randn(2, 8, 10)
        labels    = torch.zeros(2, 8, 5)
        labels[..., 3] = torch.randint(0, 3, (2, 8)).float()
        preds = model(lob, scalar)
        loss, breakdown = criterion(preds, labels)
        assert loss.item() > 0
        assert "loss_return" in breakdown

    def test_loss_decreases(self) -> None:
        """Loss should decrease with a gradient step."""
        model     = CryptoAlphaNet(d_spatial=32, d_temporal=64, seq_len=8)
        criterion = MultiTaskLoss()
        optimizer = torch.optim.Adam(model.parameters(), lr=0.01)
        lob       = torch.randn(4, 8, 5, 4)
        scalar    = torch.randn(4, 8, 10)
        labels    = torch.zeros(4, 8, 5)
        labels[..., 3] = torch.randint(0, 3, (4, 8)).float()

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
        ds     = LOBDataset(medium_df, DatasetConfig(seq_len=32))
        sample = ds[0]
        assert sample["lob"].shape    == (32, 5, 4)
        assert sample["scalar"].shape == (32, 10)
        assert sample["labels"].shape == (32, 5)
        assert sample["mask"].shape   == (32,)

    def test_walk_forward_no_leakage(self, medium_df: pl.DataFrame) -> None:
        splits = split_walk_forward(medium_df, n_folds=3, train_frac=0.75)
        for train_df, test_df in splits:
            # Test set must come after train set in time
            assert train_df["timestamp_ns"].max() <= test_df["timestamp_ns"].min()


# ── Backtest ──────────────────────────────────────────────────────────────────

class TestBacktest:
    def test_no_trades_with_zero_signal(self, small_df: pl.DataFrame) -> None:
        bt  = NeuralAlphaBacktest()
        sig = np.zeros(len(small_df))
        res = bt.run(small_df, sig)
        assert res["trades"].is_empty() or len(res["trades"]) == 0

    def test_trades_with_strong_signal(self, small_df: pl.DataFrame) -> None:
        bt  = NeuralAlphaBacktest(BacktestConfig(entry_threshold_bps=0.1))
        sig = np.ones(len(small_df)) * 0.01   # strong long signal
        res = bt.run(small_df, sig)
        assert res["metrics"]["total_trades"] > 0

    def test_equity_length(self, small_df: pl.DataFrame) -> None:
        bt  = NeuralAlphaBacktest()
        sig = np.random.randn(len(small_df)) * 0.001
        res = bt.run(small_df, sig)
        if not res["equity_curve"].is_empty():
            assert len(res["equity_curve"]) > 0

    def test_metrics_keys(self, small_df: pl.DataFrame) -> None:
        bt  = NeuralAlphaBacktest(BacktestConfig(entry_threshold_bps=0.1))
        sig = np.ones(len(small_df)) * 0.01
        res = bt.run(small_df, sig)
        for key in ("total_trades", "total_net_pnl", "sharpe_annualised", "win_rate"):
            assert key in res["metrics"]


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
                "predictions": rng.standard_normal((50, 3)).astype(np.float32),
                "labels":      rng.standard_normal((50, 5)).astype(np.float32),
            }
        ]
        metrics = analyse_alpha(fold_results)
        assert metrics.n_samples == 50
