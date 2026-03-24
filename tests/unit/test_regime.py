from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pytest

pl = pytest.importorskip("polars")

from research.regime import (
    RegimeBacktestConfig,
    RegimeConfig,
    RegimeSignalPublisher,
    infer_regime_probabilities,
    load_regime_artifact,
    run_regime_walk_forward_backtest,
    save_regime_artifact_bundle,
    save_regime_artifact,
    train_regime_model_from_df,
    train_regime_model_from_ipc,
)
from research.regime.regime import _semantic_regime_names, _stabilize_last_posterior


def _make_lob_frame(n: int = 120) -> "pl.DataFrame":
    rows: list[dict] = []
    for i in range(n):
        mid = 50000.0 + (i % 12) * 0.5
        spread = 1.0 + (i % 5) * 0.1
        rows.append(
            {
                "timestamp_ns": 1_700_000_000_000_000_000 + i,
                "best_bid": mid - spread / 2,
                "best_ask": mid + spread / 2,
                "bid_size_1": 10.0 + (i % 7),
                "ask_size_1": 9.0 + ((i + 3) % 7),
            }
        )
    return pl.DataFrame(rows)


def test_train_regime_model_reads_ipc_and_saves_artifact(tmp_path: Path) -> None:
    ipc_dir = tmp_path / "ipc"
    ipc_dir.mkdir()
    _make_lob_frame().write_parquet(ipc_dir / "lob.parquet")

    artifact, distribution = train_regime_model_from_ipc(str(ipc_dir), RegimeConfig(n_regimes=4))

    assert artifact.n_regimes == 4
    assert len(artifact.means) == 4
    assert len(artifact.variances) == 4
    assert len(artifact.transition_matrix) == 4
    assert set(distribution.keys()) == set(artifact.regime_names)
    assert abs(sum(distribution.values()) - 1.0) < 1e-6

    out = tmp_path / "models" / "r2_regime_model.json"
    save_regime_artifact(artifact, str(out))
    payload = json.loads(out.read_text(encoding="utf-8"))
    assert payload["n_regimes"] == 4
    assert payload["version"] == "r2-regime-hmm-v1"


def test_train_regime_model_rejects_invalid_count(tmp_path: Path) -> None:
    ipc_dir = tmp_path / "ipc"
    ipc_dir.mkdir()
    _make_lob_frame().write_csv(ipc_dir / "lob.csv")

    with pytest.raises(ValueError, match="3 or 4"):
        train_regime_model_from_ipc(str(ipc_dir), RegimeConfig(n_regimes=5))


def test_infer_regime_probabilities_shape(tmp_path: Path) -> None:
    ipc_dir = tmp_path / "ipc"
    ipc_dir.mkdir()
    frame = _make_lob_frame()
    frame.write_parquet(ipc_dir / "lob.parquet")

    artifact, _ = train_regime_model_from_ipc(str(ipc_dir), RegimeConfig(n_regimes=3))
    probs = infer_regime_probabilities(frame, artifact)

    assert set(probs.keys()) == {"p_calm", "p_trending", "p_shock", "p_illiquid"}
    assert abs(sum(probs.values()) - 1.0) < 1e-6


def test_hmm_artifact_roundtrip_and_stochastic_constraints(tmp_path: Path) -> None:
    ipc_dir = tmp_path / "ipc"
    ipc_dir.mkdir()
    frame = _make_lob_frame(n=180)
    frame.write_parquet(ipc_dir / "lob.parquet")

    artifact, _ = train_regime_model_from_ipc(str(ipc_dir), RegimeConfig(n_regimes=4))
    out = tmp_path / "regime_hmm.json"
    save_regime_artifact(artifact, str(out))
    loaded = load_regime_artifact(str(out))

    transition = np.array(loaded.transition_matrix)
    initial = np.array(loaded.initial_probs)

    assert np.allclose(transition.sum(axis=1), 1.0, atol=1e-6)
    assert np.all(transition >= 0.0)
    assert np.isclose(initial.sum(), 1.0, atol=1e-6)
    assert np.all(initial >= 0.0)


def test_regime_signal_publisher_writes_file(tmp_path: Path) -> None:
    signal_path = tmp_path / "regime_signal.bin"
    pub = RegimeSignalPublisher(str(signal_path))
    pub.publish(0.2, 0.3, 0.4, 0.1)

    assert signal_path.exists()
    assert signal_path.stat().st_size == 48

    pub.close()


def test_regime_signal_publisher_close_preserves_file(tmp_path: Path) -> None:
    # Bug fix regression: close() must NOT delete the signal file.
    # Deleting it creates a window during model reloads where the C++
    # RegimeSignalReader falls back to stale defaults.
    signal_path = tmp_path / "regime_signal.bin"
    pub = RegimeSignalPublisher(str(signal_path))
    pub.publish(0.5, 0.2, 0.2, 0.1)
    pub.close()

    assert signal_path.exists(), "Signal file must persist after close()"


def test_regime_signal_publisher_context_manager(tmp_path: Path) -> None:
    signal_path = tmp_path / "regime_signal.bin"
    with RegimeSignalPublisher(str(signal_path)) as pub:
        pub.publish(0.6, 0.1, 0.2, 0.1)
    assert signal_path.exists()


def test_semantic_regime_names_no_collision_3_regimes() -> None:
    # Bug fix regression: when highest-vol and highest-spread cluster coincide,
    # the old code overwrote "shock" with "illiquid", leaving no shock label
    # and disabling the C++ market-maker's shock-gating threshold.
    # Centers: cluster 2 has the highest volatility AND highest spread.
    raw_centers = np.array(
        [
            [0.01, 0.10, 5.0],  # lowest vol, lowest spread  → calm
            [0.05, 0.30, 8.0],  # mid vol,    mid spread     → illiquid (fallback)
            [0.20, 0.50, 3.0],  # highest vol, highest spread → shock (must not be overwritten)
        ]
    )
    names = _semantic_regime_names(raw_centers)
    assert len(set(names)) == 3, f"Duplicate regime names: {names}"
    assert "calm" in names
    assert "shock" in names
    assert "illiquid" in names


def test_semantic_regime_names_no_collision_4_regimes() -> None:
    # All four features point to the same cluster (cluster 3 is extreme in all).
    raw_centers = np.array(
        [
            [0.01, 0.10, 2.0, 0.0],  # calm
            [0.05, 0.20, 4.0, 0.1],
            [0.10, 0.30, 6.0, 0.2],
            [0.50, 0.80, 9.0, 0.5],  # highest vol, highest spread, highest depth
        ]
    )
    names = _semantic_regime_names(raw_centers)
    assert len(set(names)) == 4, f"Duplicate regime names: {names}"
    assert "calm" in names
    assert "shock" in names
    assert "illiquid" in names
    assert "trending" in names


def test_load_ipc_rejects_null_values(tmp_path: Path) -> None:
    ipc_dir = tmp_path / "ipc"
    ipc_dir.mkdir()
    frame = _make_lob_frame()
    # Inject a null into best_bid
    nulled = frame.with_columns(
        pl.when(pl.col("timestamp_ns") == frame["timestamp_ns"][5])
        .then(None)
        .otherwise(pl.col("best_bid"))
        .alias("best_bid")
    )
    nulled.write_parquet(ipc_dir / "lob.parquet")

    with pytest.raises(ValueError, match="Null values"):
        train_regime_model_from_ipc(str(ipc_dir), RegimeConfig(n_regimes=4))


def test_load_legacy_kmeans_artifact_upgrades_to_hmm_schema(tmp_path: Path) -> None:
    legacy = {
        "version": "r2-regime-v1",
        "n_regimes": 3,
        "feature_columns": ["vol_20", "spread_20", "depth_20", "queue_imbalance", "log_ret_1"],
        "regime_names": ["calm", "shock", "illiquid"],
        "centers": [
            [0.1, 0.2, 0.3, 0.0, 0.0],
            [0.5, 0.6, 0.2, 0.1, 0.0],
            [0.3, 0.8, 0.4, -0.1, 0.0],
        ],
        "scales": [1.0, 1.0, 1.0, 1.0, 1.0],
    }
    path = tmp_path / "legacy.json"
    path.write_text(json.dumps(legacy), encoding="utf-8")

    loaded = load_regime_artifact(str(path))

    assert loaded.version == "r2-regime-hmm-v1-legacy-upgraded"
    assert len(loaded.means) == loaded.n_regimes
    assert len(loaded.variances) == loaded.n_regimes
    assert len(loaded.initial_probs) == loaded.n_regimes
    assert len(loaded.transition_matrix) == loaded.n_regimes


def test_infer_normalizes_invalid_probabilities_in_artifact(tmp_path: Path) -> None:
    ipc_dir = tmp_path / "ipc"
    ipc_dir.mkdir()
    frame = _make_lob_frame(n=160)
    frame.write_parquet(ipc_dir / "lob.parquet")
    artifact, _ = train_regime_model_from_ipc(str(ipc_dir), RegimeConfig(n_regimes=4))

    artifact.initial_probs = [10.0, 0.0, 0.0, 0.0]
    artifact.transition_matrix = [
        [9.0, 1.0, 0.0, 0.0],
        [0.0, 7.0, 3.0, 0.0],
        [0.0, 0.0, 10.0, 0.0],
        [5.0, 0.0, 0.0, 5.0],
    ]

    probs = infer_regime_probabilities(frame, artifact)
    assert abs(sum(probs.values()) - 1.0) < 1e-6
    assert all(v >= 0.0 for v in probs.values())


def test_stabilize_last_posterior_applies_inertia() -> None:
    gamma = np.array(
        [
            [0.98, 0.01, 0.01, 0.00],
            [0.01, 0.98, 0.01, 0.00],
        ],
        dtype=np.float64,
    )
    stabilized = _stabilize_last_posterior(gamma, inertia=0.35)
    assert np.isclose(stabilized.sum(), 1.0, atol=1e-9)
    assert 0.60 < stabilized[1] < 0.98
    assert 0.01 < stabilized[0] < 0.40


def test_save_regime_artifact_bundle_writes_meta_sidecar(tmp_path: Path) -> None:
    frame = _make_lob_frame(n=160)
    artifact, _ = train_regime_model_from_df(frame, RegimeConfig(n_regimes=4))
    output = tmp_path / "regime_model.json"
    save_regime_artifact_bundle(
        artifact,
        str(output),
        metadata={"trained_at_ns": 123, "train_rows": len(frame), "artifact_version": artifact.version},
    )
    assert output.exists()
    meta_path = output.with_suffix(output.suffix + ".meta.json")
    assert meta_path.exists()
    metadata = json.loads(meta_path.read_text(encoding="utf-8"))
    assert metadata["trained_at_ns"] == 123
    assert metadata["train_rows"] == len(frame)
    loaded = load_regime_artifact(str(output))
    assert loaded.version.startswith("r2-regime-hmm-v1")


def test_regime_walk_forward_backtest_summary_is_sane() -> None:
    frame = _make_lob_frame(n=420)
    summary = run_regime_walk_forward_backtest(
        frame,
        RegimeConfig(n_regimes=4),
        RegimeBacktestConfig(train_window=160, test_window=80, step=80, min_confidence=0.55),
    )
    assert summary.windows >= 1
    assert summary.samples > 0
    assert 0.0 <= summary.mean_confidence <= 1.0
    assert summary.mean_entropy >= 0.0
    assert 0.0 <= summary.dominant_switch_rate <= 1.0
    assert 0.0 <= summary.low_confidence_rate <= 1.0
