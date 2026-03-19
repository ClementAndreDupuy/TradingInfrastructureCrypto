"""
CryptoAlphaNet — GNN spatial encoder + Transformer temporal encoder.

Architecture:
    LOB snapshot  →  LOBSpatialEncoder (attention over levels)  →  spatial embedding
    sequence      →  TemporalEncoder (Transformer)              →  temporal embedding
    concat        →  FusionLayer
    Fusion        →  MultiTaskHead
        • ReturnHead   : 4-horizon regression   (MSE)  [1t, 10t, 100t, 500t]
        • DirectionHead: up/flat/down            (cross-entropy)
        • RiskHead     : adverse-selection prob  (BCE)

Only PyTorch core — no external graph libraries.
"""
from __future__ import annotations

import math
import warnings

import torch
import torch.nn as nn
import torch.nn.functional as F

from .features import D_SCALAR, N_LEVELS

warnings.filterwarnings(
    "ignore",
    message=r"enable_nested_tensor is True, but self\.use_nested_tensor is False.*norm_first was True",
    category=UserWarning,
)
warnings.filterwarnings(
    "ignore",
    message=r"enable_nested_tensor is True, but self.use_nested_tensor is False.*norm_first was True",
    category=UserWarning,
    module=r"torch\.nn\.modules\.transformer",
)


class LOBSpatialEncoder(nn.Module):
    """
    Treats each LOB price level as a token and applies multi-head self-attention.

    Input : (B, T, N_LEVELS, 4)   — per-level [bid_p, bid_s, ask_p, ask_s]
    Output: (B, T, d_spatial)
    """

    def __init__(self, d_model: int = 64, n_heads: int = 4, n_layers: int = 2,
                 n_levels: int = N_LEVELS * 2) -> None:
        super().__init__()
        self.n_levels = n_levels  # bid + ask levels concatenated
        # Project each level (4 raw features) to d_model
        self.level_proj = nn.Linear(4, d_model)
        # Learnable level-position embedding
        self.pos_emb = nn.Embedding(n_levels, d_model)
        encoder_layer = nn.TransformerEncoderLayer(
            d_model=d_model, nhead=n_heads, dim_feedforward=d_model * 2,
            dropout=0.1, batch_first=True, norm_first=True,
        )
        self.encoder = nn.TransformerEncoder(encoder_layer, num_layers=n_layers)
        self.pool = nn.Linear(d_model, d_model)

    def forward(self, lob: torch.Tensor) -> torch.Tensor:
        """
        Args:
            lob: (B, T, N_LEVELS, 4)
        Returns:
            (B, T, d_model)
        """
        B, T, L, _ = lob.shape
        # Flatten batch and time for spatial encoding
        x = lob.view(B * T, L, 4)          # (B*T, L, 4)
        x = self.level_proj(x)              # (B*T, L, d_model)
        pos = self.pos_emb(
            torch.arange(L, device=x.device).unsqueeze(0)
        )  # (1, L, d_model)
        x = x + pos
        x = self.encoder(x)                 # (B*T, L, d_model)
        # Global average pool over levels
        spatial = x.mean(dim=1)             # (B*T, d_model)
        spatial = self.pool(spatial)        # (B*T, d_model)
        return spatial.view(B, T, -1)       # (B, T, d_model)


# ── Temporal Encoder ─────────────────────────────────────────────────────────

class TemporalEncoder(nn.Module):
    """
    Transformer over the time dimension.

    Input : (B, T, d_in)   — concatenation of spatial embed + scalar features
    Output: (B, T, d_model)
    """

    def __init__(self, d_in: int, d_model: int = 128, n_heads: int = 4,
                 n_layers: int = 3, max_seq: int = 512, dropout: float = 0.1) -> None:
        super().__init__()
        self.input_proj = nn.Linear(d_in, d_model)
        self.pos_enc = _SinusoidalPE(d_model, max_seq)
        encoder_layer = nn.TransformerEncoderLayer(
            d_model=d_model, nhead=n_heads, dim_feedforward=d_model * 4,
            dropout=dropout, batch_first=True, norm_first=True,
        )
        self.encoder = nn.TransformerEncoder(encoder_layer, num_layers=n_layers)
        self.norm = nn.LayerNorm(d_model)

    def forward(self, x: torch.Tensor, src_key_padding_mask: torch.Tensor | None = None
                ) -> torch.Tensor:
        """
        Args:
            x   : (B, T, d_in)
            mask: (B, T) bool, True = padding position to ignore
        Returns:
            (B, T, d_model)
        """
        x = self.input_proj(x)              # (B, T, d_model)
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
        self.register_buffer("pe", pe.unsqueeze(0))  # (1, max_len, d_model)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return x + self.pe[:, : x.size(1)]


# ── Multi-task Heads ──────────────────────────────────────────────────────────

class ReturnHead(nn.Module):
    """Regress multi-horizon log-returns."""

    def __init__(self, d_in: int, n_horizons: int = 4) -> None:
        super().__init__()
        self.net = nn.Sequential(
            nn.Linear(d_in, d_in // 2),
            nn.GELU(),
            nn.Linear(d_in // 2, n_horizons),
        )

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return self.net(x)


class DirectionHead(nn.Module):
    """Classify trend: 0=down, 1=flat, 2=up."""

    def __init__(self, d_in: int, n_classes: int = 3) -> None:
        super().__init__()
        self.net = nn.Sequential(
            nn.Linear(d_in, d_in // 2),
            nn.GELU(),
            nn.Linear(d_in // 2, n_classes),
        )

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return self.net(x)  # logits


class RiskHead(nn.Module):
    """Estimate adverse-selection probability."""

    def __init__(self, d_in: int) -> None:
        super().__init__()
        self.net = nn.Sequential(
            nn.Linear(d_in, d_in // 4),
            nn.GELU(),
            nn.Linear(d_in // 4, 1),
        )

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return torch.sigmoid(self.net(x)).squeeze(-1)


# ── Full Model ────────────────────────────────────────────────────────────────

class CryptoAlphaNet(nn.Module):
    """
    End-to-end model that takes LOB snapshots + scalar features and outputs
    return predictions, direction logits, and risk scores.

    Args:
        d_spatial   : hidden size for spatial encoder
        d_temporal  : hidden size for temporal encoder
        n_lob_heads : attention heads in spatial encoder
        n_lob_layers: transformer layers in spatial encoder
        n_temp_heads: attention heads in temporal encoder
        n_temp_layers: transformer layers in temporal encoder
        n_horizons  : number of return horizons (default 4: 1t, 10t, 100t, 500t)
        seq_len     : max sequence length (for positional encoding)
        dropout     : dropout rate
    """

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
        self.spatial_enc = LOBSpatialEncoder(
            d_model=d_spatial,
            n_heads=n_lob_heads,
            n_layers=n_lob_layers,
            n_levels=n_levels,
        )
        d_fused_in = d_spatial + d_scalar
        self.temporal_enc = TemporalEncoder(
            d_in=d_fused_in,
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
        )
        self.return_head    = ReturnHead(d_temporal, n_horizons)
        self.direction_head = DirectionHead(d_temporal)
        self.risk_head      = RiskHead(d_temporal)

    def forward(
        self,
        lob: torch.Tensor,
        scalar: torch.Tensor,
        mask: torch.Tensor | None = None,
    ) -> dict[str, torch.Tensor]:
        """
        Args:
            lob   : (B, T, N_LEVELS, 4)
            scalar: (B, T, D_SCALAR)
            mask  : (B, T) padding mask (True = ignore)
        Returns:
            dict with keys: returns (B,T,4), direction (B,T,3), risk (B,T)
        """
        if lob.ndim != 4:
            raise ValueError(f"lob must have shape (B, T, N_LEVELS, 4), got {tuple(lob.shape)}")
        if scalar.ndim != 3:
            raise ValueError(f"scalar must have shape (B, T, D_SCALAR), got {tuple(scalar.shape)}")
        if lob.shape[0] != scalar.shape[0] or lob.shape[1] != scalar.shape[1]:
            raise ValueError("lob and scalar batch/time dimensions must match")
        if lob.shape[-1] != 4:
            raise ValueError("lob last dimension must be 4: [bid_px,bid_sz,ask_px,ask_sz]")

        spatial = self.spatial_enc(lob)              # (B, T, d_spatial)
        x = torch.cat([spatial, scalar], dim=-1)     # (B, T, d_spatial + D_SCALAR)
        temporal = self.temporal_enc(x, mask)        # (B, T, d_temporal)
        fused = self.fusion(temporal)                # (B, T, d_temporal)

        return {
            "returns":   self.return_head(fused),    # (B, T, n_horizons)
            "direction": self.direction_head(fused), # (B, T, 3)
            "risk":      self.risk_head(fused),      # (B, T)
        }


# ── Multi-task Loss ───────────────────────────────────────────────────────────

class MultiTaskLoss(nn.Module):
    """
    Weighted combination of:
        - MSE for return regression (per horizon)
        - Cross-entropy for direction classification
        - BCE for adverse-selection risk
        - Transaction-cost penalty (penalises large predicted returns near zero)

    Targets layout (B, T, 6):
        cols 0-3: returns at horizons [1t, 10t, 100t, 500t]
        col  4  : direction (0/1/2)
        col  5  : adverse selection (0/1)
    """

    def __init__(
        self,
        w_return: float = 1.0,
        w_direction: float = 0.5,
        w_risk: float = 0.3,
        w_tc: float = 0.1,
        tc_bps: float = 7.0,         # round-trip cost in bps
        label_smoothing: float = 0.05,
    ) -> None:
        super().__init__()
        self.w_return    = w_return
        self.w_direction = w_direction
        self.w_risk      = w_risk
        self.w_tc        = w_tc
        self.tc_threshold = tc_bps * 1e-4  # convert bps → fraction

        self.ce_loss  = nn.CrossEntropyLoss(label_smoothing=label_smoothing)
        self.bce_loss = nn.BCELoss()

    def forward(
        self,
        preds: dict[str, torch.Tensor],
        targets: torch.Tensor,
    ) -> tuple[torch.Tensor, dict[str, float]]:
        """
        Args:
            preds  : dict from CryptoAlphaNet.forward
            targets: (B, T, 6) — [ret_1, ret_10, ret_100, ret_500, direction, adv_sel]
        Returns:
            total_loss, breakdown_dict
        """
        ret_targets  = targets[..., :4]          # 4 horizons
        dir_targets  = targets[..., 4].long()
        risk_targets = targets[..., 5]

        # Return loss (MSE, average across horizons)
        ret_loss = F.mse_loss(preds["returns"], ret_targets)

        # Direction loss (cross-entropy on flattened batch×time)
        B, T, _ = preds["direction"].shape
        dir_loss = self.ce_loss(
            preds["direction"].view(B * T, -1),
            dir_targets.view(B * T),
        )

        # Risk loss (BCE)
        risk_loss = self.bce_loss(preds["risk"], risk_targets)

        # Transaction-cost penalty: penalise predictions too small to trade profitably
        pred_ret_mid = preds["returns"][..., 2]  # mid-horizon (index 2 = 100t)
        tc_penalty = F.relu(self.tc_threshold - pred_ret_mid.abs()).mean()

        total = (
            self.w_return    * ret_loss
            + self.w_direction * dir_loss
            + self.w_risk      * risk_loss
            + self.w_tc        * tc_penalty
        )

        breakdown = {
            "loss_return":    ret_loss.item(),
            "loss_direction": dir_loss.item(),
            "loss_risk":      risk_loss.item(),
            "loss_tc":        tc_penalty.item(),
            "loss_total":     total.item(),
        }
        return total, breakdown
