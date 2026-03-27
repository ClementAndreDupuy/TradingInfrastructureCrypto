"""
M4: Live-capture integration validation for ensemble checkpoints against core IPC feed snapshots.

This test validates that both primary and secondary CryptoAlphaNet checkpoints
produce valid inference outputs when fed real LOB snapshots from the core IPC
ring buffer.  No mocked or synthetic data paths are permitted — if the IPC feed
is unavailable the test is skipped rather than falling back to fabricated data.

Configuration via environment variables:
    TRT_PRIMARY_CHECKPOINT    Path to primary model .pt file
                              (default: models/neural_alpha_btcusdt_latest.pt)
    TRT_SECONDARY_CHECKPOINT  Path to secondary model .pt file
                              (default: models/neural_alpha_btcusdt_secondary.pt)
    TRT_IPC_FEED_PATH         Override for IPC ring-buffer path
                              (default: /tmp/trt_lob_feed.bin)
    TRT_IPC_MIN_TICKS         Minimum live ticks to capture before inference
                              (default: 64, matching default seq_len)
    TRT_IPC_TIMEOUT_MS        Maximum milliseconds to wait for ticks
                              (default: 5000)
"""

from __future__ import annotations

import os
import sys
import time
from pathlib import Path

import numpy as np
import polars as pl
import pytest
import torch

ROOT = Path(__file__).parent.parent.parent
sys.path.insert(0, str(ROOT))

from research.neural_alpha.runtime.core_bridge import CoreBridge, RING_PATH
from research.neural_alpha.data.dataset import DatasetConfig, LOBDataset
from research.neural_alpha.models.model import CryptoAlphaNet

# ── Defaults ──────────────────────────────────────────────────────────────────

_DEFAULT_PRIMARY = ROOT / "models" / "neural_alpha_btcusdt_latest.pt"
_DEFAULT_SECONDARY = ROOT / "models" / "neural_alpha_btcusdt_secondary.pt"
_DEFAULT_MIN_TICKS = 64
_DEFAULT_TIMEOUT_MS = 5_000


# ── Configuration helpers ─────────────────────────────────────────────────────


def _ipc_path() -> str:
    return os.environ.get("TRT_IPC_FEED_PATH", RING_PATH)


def _min_ticks() -> int:
    return int(os.environ.get("TRT_IPC_MIN_TICKS", _DEFAULT_MIN_TICKS))


def _timeout_ms() -> int:
    return int(os.environ.get("TRT_IPC_TIMEOUT_MS", _DEFAULT_TIMEOUT_MS))


def _primary_checkpoint() -> Path:
    return Path(os.environ.get("TRT_PRIMARY_CHECKPOINT", str(_DEFAULT_PRIMARY)))


def _secondary_checkpoint() -> Path:
    return Path(os.environ.get("TRT_SECONDARY_CHECKPOINT", str(_DEFAULT_SECONDARY)))


# ── IPC capture ───────────────────────────────────────────────────────────────


def _capture_ipc_ticks(min_ticks: int, timeout_ms: int) -> list[dict]:
    """
    Drain new ticks from the live IPC ring buffer.

    Returns a list of tick dicts (possibly empty when the feed file is absent).
    Never substitutes synthetic or REST data.
    """
    bridge = CoreBridge(path=_ipc_path())
    if not bridge.open():
        return []

    rows: list[dict] = []
    deadline_ns = time.monotonic_ns() + timeout_ms * 1_000_000
    poll_s = 0.05

    try:
        while len(rows) < min_ticks:
            rows.extend(bridge.read_new_ticks())
            if len(rows) >= min_ticks:
                break
            if time.monotonic_ns() > deadline_ns:
                break
            time.sleep(poll_s)
    finally:
        bridge.close()

    return rows


# ── Checkpoint loading ────────────────────────────────────────────────────────


def _load_checkpoint(path: Path, seq_len: int) -> CryptoAlphaNet | None:
    """
    Load a CryptoAlphaNet from a saved state-dict checkpoint.

    Architecture dimensions (d_spatial, d_temporal) are inferred from the
    checkpoint itself so the test is not coupled to any hard-coded defaults.
    Returns None when the checkpoint file does not exist.
    """
    if not path.exists():
        return None

    state: dict[str, torch.Tensor] = torch.load(path, map_location="cpu", weights_only=True)

    # Infer spatial dim from the learnable level-position embedding
    d_spatial = int(state["spatial_enc.pos_emb.weight"].shape[1])
    # Infer temporal dim from the final LayerNorm weight
    d_temporal = int(state["temporal_enc.norm.weight"].shape[0])

    model = CryptoAlphaNet(d_spatial=d_spatial, d_temporal=d_temporal, seq_len=seq_len)
    model.load_state_dict(state, strict=True)
    model.eval()
    return model


# ── Inference ─────────────────────────────────────────────────────────────────


def _infer_last_window(
    model: CryptoAlphaNet, df: pl.DataFrame, seq_len: int
) -> dict[str, torch.Tensor]:
    """
    Build feature tensors from df and run the model on the most recent window.

    The last available sliding window is used so the inference reflects the
    most recent market state captured from the IPC feed.
    """
    dataset = LOBDataset(df, DatasetConfig(seq_len=seq_len))
    if len(dataset) == 0:
        raise RuntimeError(
            f"LOBDataset is empty for seq_len={seq_len} with {len(df)} ticks — "
            "increase TRT_IPC_MIN_TICKS."
        )

    sample = dataset[len(dataset) - 1]
    lob = sample["lob"].unsqueeze(0)  # (1, seq_len, N_LEVELS, D_LOB)
    scalar = sample["scalar"].unsqueeze(0)  # (1, seq_len, D_SCALAR)

    with torch.no_grad():
        return model(lob, scalar)


# ── Output assertions ─────────────────────────────────────────────────────────


def _assert_valid_outputs(preds: dict[str, torch.Tensor], label: str, seq_len: int) -> None:
    """Assert shape correctness, finiteness, and risk boundedness."""
    ret = preds["returns"]  # (1, seq_len, 4)
    dirn = preds["direction"]  # (1, seq_len, 3)
    risk = preds["risk"]  # (1, seq_len)

    assert ret.shape == (
        1,
        seq_len,
        4,
    ), f"{label} returns: expected (1, {seq_len}, 4), got {tuple(ret.shape)}"
    assert dirn.shape == (
        1,
        seq_len,
        3,
    ), f"{label} direction: expected (1, {seq_len}, 3), got {tuple(dirn.shape)}"
    assert risk.shape == (
        1,
        seq_len,
    ), f"{label} risk: expected (1, {seq_len}), got {tuple(risk.shape)}"

    assert torch.isfinite(ret).all(), f"{label} returns contain non-finite values"
    assert torch.isfinite(dirn).all(), f"{label} direction logits contain non-finite values"
    assert torch.isfinite(risk).all(), f"{label} risk scores contain non-finite values"

    r_min, r_max = float(risk.min()), float(risk.max())
    assert (
        r_min >= 0.0 and r_max <= 1.0
    ), f"{label} risk not in [0, 1]: min={r_min:.4f} max={r_max:.4f}"


# ── Integration test ──────────────────────────────────────────────────────────


def test_ensemble_checkpoint_inference_on_live_ipc_feed() -> None:
    """
    End-to-end integration test for M4.

    Steps
    -----
    1. Open the core IPC ring buffer (no mock, no REST fallback).
    2. Capture ≥ TRT_IPC_MIN_TICKS LOB snapshots within TRT_IPC_TIMEOUT_MS.
    3. Load the primary and/or secondary ensemble checkpoint(s).
    4. Run forward inference on the last seq_len window of live data.
    5. Assert: output shapes, finiteness, risk ∈ [0, 1].
    6. If both checkpoints are present, assert the 50/50 blended ensemble
       predictions are also finite and risk-bounded.

    Skip conditions (not failures)
    --------------------------------
    - IPC ring buffer file absent (feed process not running).
    - Fewer ticks than TRT_IPC_MIN_TICKS collected within the timeout.
    - Neither checkpoint file exists (model not yet trained).

    Failure conditions
    ------------------
    - A checkpoint file exists but contains non-finite or out-of-range outputs.
    - The checkpoint state dict is structurally incompatible with CryptoAlphaNet.
    """
    seq_len = _min_ticks()
    timeout_ms = _timeout_ms()

    # ── Step 1–2: Live IPC capture ───────────────────────────────────────────
    ticks = _capture_ipc_ticks(min_ticks=seq_len, timeout_ms=timeout_ms)

    if not ticks:
        pytest.skip(
            f"IPC ring buffer unavailable at {_ipc_path()!r}. "
            "Start the core feed process to enable this integration test."
        )

    if len(ticks) < seq_len:
        pytest.skip(
            f"IPC feed provided only {len(ticks)} ticks within {timeout_ms} ms "
            f"(need {seq_len}). "
            "Increase TRT_IPC_TIMEOUT_MS or reduce TRT_IPC_MIN_TICKS."
        )

    df = pl.DataFrame(ticks).sort("timestamp_ns")

    # ── Step 3: Checkpoint loading ───────────────────────────────────────────
    primary_path = _primary_checkpoint()
    secondary_path = _secondary_checkpoint()

    primary_model = _load_checkpoint(primary_path, seq_len=seq_len)
    secondary_model = _load_checkpoint(secondary_path, seq_len=seq_len)

    if primary_model is None and secondary_model is None:
        pytest.skip(
            f"No checkpoints found at {primary_path!r} or {secondary_path!r}. "
            "Train a model first or configure TRT_PRIMARY_CHECKPOINT / "
            "TRT_SECONDARY_CHECKPOINT."
        )

    # ── Steps 4–5: Per-model inference and output validation ─────────────────
    primary_preds = None
    secondary_preds = None

    if primary_model is not None:
        primary_preds = _infer_last_window(primary_model, df, seq_len=seq_len)
        _assert_valid_outputs(primary_preds, label="primary", seq_len=seq_len)

    if secondary_model is not None:
        secondary_preds = _infer_last_window(secondary_model, df, seq_len=seq_len)
        _assert_valid_outputs(secondary_preds, label="secondary", seq_len=seq_len)

    # ── Step 6: Ensemble blend validation ────────────────────────────────────
    if primary_preds is not None and secondary_preds is not None:
        ensemble_ret = (primary_preds["returns"] + secondary_preds["returns"]) / 2.0
        ensemble_dirn = (primary_preds["direction"] + secondary_preds["direction"]) / 2.0
        ensemble_risk = (primary_preds["risk"] + secondary_preds["risk"]) / 2.0

        assert torch.isfinite(ensemble_ret).all(), "Ensemble returns contain non-finite values"
        assert torch.isfinite(
            ensemble_dirn
        ).all(), "Ensemble direction logits contain non-finite values"
        assert torch.isfinite(ensemble_risk).all(), "Ensemble risk scores contain non-finite values"

        e_min, e_max = float(ensemble_risk.min()), float(ensemble_risk.max())
        assert (
            e_min >= 0.0 and e_max <= 1.0
        ), f"Ensemble risk not in [0, 1]: min={e_min:.4f} max={e_max:.4f}"
