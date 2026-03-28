from __future__ import annotations

import pytest
import torch

from research.neural_alpha.data.features import D_LOB, D_SCALAR, N_LEVELS
from research.neural_alpha.models.model import CryptoAlphaNet
from research.neural_alpha.runtime.shadow_session import NeuralAlphaShadowSession


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


def test_migrate_state_dict_pads_old_lob_and_scalar_dims() -> None:
    old_d_lob = D_LOB - 2
    old_d_scalar = D_SCALAR - 2
    old_model = CryptoAlphaNet(d_lob_in=old_d_lob, d_scalar=old_d_scalar)
    old_state = old_model.state_dict()

    new_model = CryptoAlphaNet()
    migrated = NeuralAlphaShadowSession._migrate_state_dict(old_state, new_model)
    new_model.load_state_dict(migrated)

    lp = new_model.spatial_enc.level_proj
    assert lp.weight.shape == (64, D_LOB)
    assert torch.allclose(lp.weight[:, :old_d_lob], old_model.spatial_enc.level_proj.weight)
    assert lp.weight[:, old_d_lob:].eq(0).all()

    ip = new_model.temporal_enc.input_proj
    old_d_in = 64 + old_d_scalar
    new_d_in = 64 + D_SCALAR
    assert ip.weight.shape == (128, new_d_in)
    assert torch.allclose(ip.weight[:, :old_d_in], old_model.temporal_enc.input_proj.weight)
    assert ip.weight[:, old_d_in:].eq(0).all()


def test_migrate_state_dict_is_noop_when_shapes_match() -> None:
    model = CryptoAlphaNet()
    state = model.state_dict()
    migrated = NeuralAlphaShadowSession._migrate_state_dict(state, model)
    for key in state:
        assert torch.equal(state[key], migrated[key])
