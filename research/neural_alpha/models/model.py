from __future__ import annotations

import math
import warnings

import torch
import torch.nn as nn
import torch.nn.functional as F

from ..data.features import D_SCALAR, N_LEVELS

warnings.filterwarnings(
    "ignore",
    message="enable_nested_tensor is True, but self\\.use_nested_tensor is False.*norm_first was True",
    category=UserWarning,
)
warnings.filterwarnings(
    "ignore",
    message="enable_nested_tensor is True, but self.use_nested_tensor is False.*norm_first was True",
    category=UserWarning,
    module="torch\\.nn\\.modules\\.transformer",
)


class LOBSpatialEncoder(nn.Module):
    """Encode per-level LOB state with self-attention over price levels."""

    def __init__(
        self,
        d_model: int = 64,
        n_heads: int = 4,
        n_layers: int = 2,
        n_levels: int = N_LEVELS,
        dropout: float = 0.1,
    ) -> None:
        super().__init__()
        self.n_levels = n_levels
        self.level_proj = nn.Linear(4, d_model)
        self.pos_emb = nn.Embedding(n_levels, d_model)
        encoder_layer = nn.TransformerEncoderLayer(
            d_model=d_model,
            nhead=n_heads,
            dim_feedforward=d_model * 2,
            dropout=dropout,
            batch_first=True,
            norm_first=True,
        )
        self.encoder = nn.TransformerEncoder(encoder_layer, num_layers=n_layers)
        self.pool = nn.Sequential(nn.LayerNorm(d_model), nn.Linear(d_model, d_model))

    def forward(self, lob: torch.Tensor) -> torch.Tensor:
        (batch, seq_len, levels, _) = lob.shape
        x = lob.reshape(batch * seq_len, levels, 4)
        x = self.level_proj(x)
        pos = self.pos_emb(torch.arange(levels, device=x.device)).unsqueeze(0)
        x = self.encoder(x + pos)
        return self.pool(x.mean(dim=1)).reshape(batch, seq_len, -1)


class TemporalEncoder(nn.Module):
    """Transformer over time after fusing spatial and scalar features."""

    def __init__(
        self,
        d_in: int,
        d_model: int = 128,
        n_heads: int = 4,
        n_layers: int = 3,
        max_seq: int = 512,
        dropout: float = 0.1,
    ) -> None:
        super().__init__()
        self.input_proj = nn.Linear(d_in, d_model)
        self.pos_enc = _SinusoidalPE(d_model, max_seq)
        encoder_layer = nn.TransformerEncoderLayer(
            d_model=d_model,
            nhead=n_heads,
            dim_feedforward=d_model * 4,
            dropout=dropout,
            batch_first=True,
            norm_first=True,
        )
        self.encoder = nn.TransformerEncoder(encoder_layer, num_layers=n_layers)
        self.norm = nn.LayerNorm(d_model)

    def forward(
        self, x: torch.Tensor, src_key_padding_mask: torch.Tensor | None = None
    ) -> torch.Tensor:
        x = self.input_proj(x)
        x = self.pos_enc(x)
        x = self.encoder(x, src_key_padding_mask=src_key_padding_mask)
        return self.norm(x)


class _SinusoidalPE(nn.Module):
    def __init__(self, d_model: int, max_len: int = 512) -> None:
        super().__init__()
        pe = torch.zeros(max_len, d_model)
        pos = torch.arange(max_len).unsqueeze(1).float()
        div = torch.exp(torch.arange(0, d_model, 2).float() * (-math.log(10000.0) / d_model))
        pe[:, 0::2] = torch.sin(pos * div)
        pe[:, 1::2] = torch.cos(pos * div)
        self.register_buffer("pe", pe.unsqueeze(0))

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return x + self.pe[:, : x.size(1)]


class MLPHead(nn.Module):
    """Small shared head used for all prediction tasks."""

    def __init__(self, d_in: int, d_hidden: int, d_out: int) -> None:
        super().__init__()
        self.net = nn.Sequential(nn.Linear(d_in, d_hidden), nn.GELU(), nn.Linear(d_hidden, d_out))

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return self.net(x)


class CryptoAlphaNet(nn.Module):
    """LOB encoder + temporal encoder + lightweight multitask heads."""

    def __init__(
        self,
        d_spatial: int = 64,
        d_temporal: int = 128,
        n_lob_heads: int = 4,
        n_lob_layers: int = 2,
        n_temp_heads: int = 4,
        n_temp_layers: int = 3,
        n_horizons: int = 4,
        seq_len: int = 512,
        dropout: float = 0.1,
        d_scalar: int = D_SCALAR,
        n_levels: int = N_LEVELS,
    ) -> None:
        super().__init__()
        self.n_levels = n_levels
        self.d_scalar = d_scalar
        self.spatial_enc = LOBSpatialEncoder(
            d_model=d_spatial,
            n_heads=n_lob_heads,
            n_layers=n_lob_layers,
            n_levels=n_levels,
            dropout=dropout,
        )
        self.temporal_enc = TemporalEncoder(
            d_in=d_spatial + d_scalar,
            d_model=d_temporal,
            n_heads=n_temp_heads,
            n_layers=n_temp_layers,
            max_seq=seq_len,
            dropout=dropout,
        )
        self.fusion = nn.Sequential(
            nn.Linear(d_temporal, d_temporal),
            nn.GELU(),
            nn.Dropout(dropout),
            nn.LayerNorm(d_temporal),
        )
        self.return_head = MLPHead(d_temporal, d_temporal // 2, n_horizons)
        self.direction_head = MLPHead(d_temporal, d_temporal // 2, 3)
        self.risk_head = MLPHead(d_temporal, max(8, d_temporal // 4), 1)

    def forward(
        self, lob: torch.Tensor, scalar: torch.Tensor, mask: torch.Tensor | None = None
    ) -> dict[str, torch.Tensor]:
        if lob.ndim != 4:
            raise ValueError(f"lob must have shape (B, T, N_LEVELS, 4), got {tuple(lob.shape)}")
        if scalar.ndim != 3:
            raise ValueError(f"scalar must have shape (B, T, D_SCALAR), got {tuple(scalar.shape)}")
        if lob.shape[0] != scalar.shape[0] or lob.shape[1] != scalar.shape[1]:
            raise ValueError("lob and scalar batch/time dimensions must match")
        if lob.shape[2] != self.n_levels:
            raise ValueError(
                f"lob level dimension must be {self.n_levels} to match IPC LOB depth, "
                f"got {lob.shape[2]}"
            )
        if lob.shape[-1] != 4:
            raise ValueError("lob last dimension must be 4: [bid_px,bid_sz,ask_px,ask_sz]")
        if scalar.shape[-1] != self.d_scalar:
            raise ValueError(
                f"scalar last dimension must be {self.d_scalar} to match feature pipeline, "
                f"got {scalar.shape[-1]}"
            )

        spatial = self.spatial_enc(lob)
        fused = self.fusion(self.temporal_enc(torch.cat([spatial, scalar], dim=-1), mask))
        risk_logits = self.risk_head(fused).squeeze(-1)
        return {
            "returns": self.return_head(fused),
            "direction": self.direction_head(fused),
            "risk_logits": risk_logits,
            "risk": torch.sigmoid(risk_logits),
        }


class MultiTaskLoss(nn.Module):
    """Robust multitask loss for noisy return, direction, and risk targets."""

    def __init__(
        self,
        w_return: float = 1.0,
        w_direction: float = 0.5,
        w_risk: float = 0.3,
        w_tc: float = 0.1,
        tc_bps: float = 7.0,
        label_smoothing: float = 0.05,
        return_beta: float = 5e-5,
        direction_class_weights: torch.Tensor | None = None,
        risk_pos_weight: torch.Tensor | None = None,
    ) -> None:
        super().__init__()
        self.w_return = w_return
        self.w_direction = w_direction
        self.w_risk = w_risk
        self.w_tc = w_tc
        self.tc_threshold = tc_bps * 0.0001
        self.return_loss = nn.SmoothL1Loss(beta=return_beta)
        self.register_buffer("direction_class_weights", direction_class_weights)
        self.register_buffer("risk_pos_weight", risk_pos_weight)
        self.ce_loss = nn.CrossEntropyLoss(
            weight=self.direction_class_weights,
            label_smoothing=label_smoothing,
        )
        self.bce_loss = nn.BCEWithLogitsLoss(pos_weight=self.risk_pos_weight)

    def forward(
        self, preds: dict[str, torch.Tensor], targets: torch.Tensor
    ) -> tuple[torch.Tensor, dict[str, float]]:
        ret_targets = targets[..., :4]
        dir_targets = targets[..., 4].long()
        risk_targets = targets[..., 5]

        ret_loss = self.return_loss(preds["returns"], ret_targets)
        (batch, seq_len, _) = preds["direction"].shape
        dir_loss = self.ce_loss(preds["direction"].reshape(batch * seq_len, -1), dir_targets.reshape(batch * seq_len))
        risk_logits = preds.get("risk_logits")
        if risk_logits is None:
            risk_logits = torch.logit(preds["risk"].clamp(1e-6, 1.0 - 1e-6))
        risk_loss = self.bce_loss(risk_logits, risk_targets)
        pred_ret_mid = preds["returns"][..., 2]
        tc_penalty = F.relu(self.tc_threshold - pred_ret_mid.abs()).mean()

        total = (
            self.w_return * ret_loss
            + self.w_direction * dir_loss
            + self.w_risk * risk_loss
            + self.w_tc * tc_penalty
        )
        breakdown = {
            "loss_return": float(ret_loss.detach().item()),
            "loss_direction": float(dir_loss.detach().item()),
            "loss_risk": float(risk_loss.detach().item()),
            "loss_tc": float(tc_penalty.detach().item()),
            "loss_total": float(total.detach().item()),
        }
        return total, breakdown
