from __future__ import annotations

import pytest
import torch

from research.neural_alpha.data.features import D_LOB, D_SCALAR, N_LEVELS
from research.neural_alpha.models.model import CryptoAlphaNet


def test_model_accepts_lob_publisher_depth_and_scalar_schema() -> None:
    model = CryptoAlphaNet()
    lob = torch.randn(2, 16, N_LEVELS, D_LOB)
    scalar = torch.randn(2, 16, D_SCALAR)
    out = model(lob, scalar)
    assert out["returns"].shape == (2, 16, 4)


def test_model_rejects_legacy_lob_depth_shape() -> None:
    model = CryptoAlphaNet()
    legacy_lob = torch.randn(1, 8, 5, D_LOB)
    scalar = torch.randn(1, 8, D_SCALAR)
    with pytest.raises(ValueError, match="lob level dimension must be"):
        model(legacy_lob, scalar)


def test_model_rejects_scalar_schema_mismatch() -> None:
    model = CryptoAlphaNet()
    lob = torch.randn(1, 8, N_LEVELS, D_LOB)
    bad_scalar = torch.randn(1, 8, D_SCALAR - 1)
    with pytest.raises(ValueError, match="scalar last dimension must be"):
        model(lob, bad_scalar)
