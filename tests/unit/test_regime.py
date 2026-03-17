from __future__ import annotations

import json
from pathlib import Path

import pytest

pl = pytest.importorskip("polars")

from research.neural_alpha.regime import (
    RegimeConfig,
    RegimeSignalPublisher,
    infer_regime_probabilities,
    save_regime_artifact,
    train_regime_model_from_ipc,
)


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
    assert len(artifact.centers) == 4
    assert set(distribution.keys()) == set(artifact.regime_names)
    assert abs(sum(distribution.values()) - 1.0) < 1e-6

    out = tmp_path / "models" / "r2_regime_model.json"
    save_regime_artifact(artifact, str(out))
    payload = json.loads(out.read_text(encoding="utf-8"))
    assert payload["n_regimes"] == 4
    assert payload["version"] == "r2-regime-v1"


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


def test_regime_signal_publisher_writes_file(tmp_path: Path) -> None:
    signal_path = tmp_path / "regime_signal.bin"
    pub = RegimeSignalPublisher(str(signal_path))
    pub.publish(0.2, 0.3, 0.4, 0.1)

    assert signal_path.exists()
    assert signal_path.stat().st_size == 48

    pub.close()
