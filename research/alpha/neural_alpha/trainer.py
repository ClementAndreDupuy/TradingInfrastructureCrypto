"""
Walk-forward trainer for CryptoAlphaNet.

One fold:
    1. Train on training split (multi-task loss)
    2. Evaluate on test split (collect predictions + metrics)
    3. Return per-tick predictions for downstream backtest and alpha regression

Contrastive pre-training:
    Before supervised training, optionally run self-supervised contrastive
    pre-training on the LOB encoder using augmented LOB views (random level
    dropout + Gaussian noise).
"""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
from torch.utils.data import DataLoader

from .dataset import DatasetConfig, build_loaders, split_walk_forward
from .model import CryptoAlphaNet, MultiTaskLoss


@dataclass
class TrainerConfig:
    # Model
    d_spatial: int = 64
    d_temporal: int = 128
    n_lob_heads: int = 4
    n_lob_layers: int = 2
    n_temp_heads: int = 4
    n_temp_layers: int = 3
    dropout: float = 0.1
    seq_len: int = 64

    # Training
    epochs: int = 20
    batch_size: int = 32
    lr: float = 3e-4
    weight_decay: float = 1e-4
    grad_clip: float = 1.0

    # Walk-forward
    n_folds: int = 4
    train_frac: float = 0.75

    # Pre-training
    pretrain_epochs: int = 5
    pretrain: bool = True

    # Loss weights
    w_return: float = 1.0
    w_direction: float = 0.5
    w_risk: float = 0.3
    w_tc: float = 0.1
    tc_bps: float = 7.0

    # Adversarial noise
    adv_noise_std: float = 0.02
    resume_state_dict: dict[str, torch.Tensor] | None = None


def _device() -> torch.device:
    return torch.device("cuda" if torch.cuda.is_available() else "cpu")


# ── Contrastive pre-training ─────────────────────────────────────────────────

def _augment_lob(lob: torch.Tensor, noise_std: float = 0.02) -> torch.Tensor:
    """Random Gaussian noise + random level masking."""
    x = lob + torch.randn_like(lob) * noise_std
    # randomly zero out one level
    if lob.shape[2] > 1:
        mask_level = torch.randint(0, lob.shape[2], (lob.shape[0], lob.shape[1]))
        for b in range(lob.shape[0]):
            for t in range(lob.shape[1]):
                x[b, t, mask_level[b, t]] = 0.0
    return x


def pretrain_spatial(
    model: CryptoAlphaNet,
    loader: DataLoader,
    epochs: int,
    lr: float,
    device: torch.device,
    noise_std: float = 0.02,
) -> None:
    """
    Contrastive self-supervised pre-training on the spatial encoder.
    Uses NT-Xent loss on two augmented views of each LOB snapshot.
    """
    optimizer = torch.optim.Adam(model.spatial_enc.parameters(), lr=lr)
    model.spatial_enc.train()
    temperature = 0.07

    for epoch in range(epochs):
        total_loss = 0.0
        n_batches = 0
        for batch in loader:
            lob = batch["lob"].to(device)  # (B, T, L, 4)

            view1 = _augment_lob(lob, noise_std)
            view2 = _augment_lob(lob, noise_std)

            # Encode both views, pool over time
            z1 = model.spatial_enc(view1).mean(dim=1)  # (B, d_spatial)
            z2 = model.spatial_enc(view2).mean(dim=1)

            # NT-Xent loss (SimCLR)
            z1 = F.normalize(z1, dim=-1)
            z2 = F.normalize(z2, dim=-1)
            B = z1.shape[0]
            z = torch.cat([z1, z2], dim=0)           # (2B, d)
            sim = torch.mm(z, z.T) / temperature      # (2B, 2B)
            # Mask self-similarity
            mask = torch.eye(2 * B, device=device).bool()
            sim.masked_fill_(mask, float("-inf"))
            labels = torch.cat([
                torch.arange(B, 2 * B, device=device),
                torch.arange(0, B, device=device),
            ])
            loss = F.cross_entropy(sim, labels)

            optimizer.zero_grad()
            loss.backward()
            optimizer.step()
            total_loss += loss.item()
            n_batches += 1

        if n_batches > 0:
            print(f"  [pretrain] epoch {epoch+1}/{epochs}  loss={total_loss/n_batches:.4f}")


# ── Supervised training ───────────────────────────────────────────────────────

def train_epoch(
    model: CryptoAlphaNet,
    loader: DataLoader,
    optimizer: torch.optim.Optimizer,
    criterion: MultiTaskLoss,
    device: torch.device,
    grad_clip: float = 1.0,
    noise_std: float = 0.0,
) -> dict[str, float]:
    model.train()
    totals: dict[str, float] = {}
    n = 0

    for batch in loader:
        lob    = batch["lob"].to(device)
        scalar = batch["scalar"].to(device)
        labels = batch["labels"].to(device)
        mask   = batch["mask"].to(device)

        # Adversarial noise augmentation
        if noise_std > 0:
            lob    = lob + torch.randn_like(lob) * noise_std
            scalar = scalar + torch.randn_like(scalar) * noise_std * 0.1

        preds = model(lob, scalar, mask)
        loss, breakdown = criterion(preds, labels)

        optimizer.zero_grad()
        loss.backward()
        nn.utils.clip_grad_norm_(model.parameters(), grad_clip)
        optimizer.step()

        for k, v in breakdown.items():
            totals[k] = totals.get(k, 0.0) + v
        n += 1

    return {k: v / max(n, 1) for k, v in totals.items()}


@torch.no_grad()
def eval_epoch(
    model: CryptoAlphaNet,
    loader: DataLoader,
    criterion: MultiTaskLoss,
    device: torch.device,
) -> tuple[dict[str, float], np.ndarray, np.ndarray]:
    """
    Returns:
        metrics     — loss breakdown
        all_preds   — (N_ticks, 3) return predictions for the mid-horizon
        all_labels  — (N_ticks, 5) ground truth
    """
    model.eval()
    totals: dict[str, float] = {}
    n = 0
    pred_list: list[np.ndarray] = []
    label_list: list[np.ndarray] = []

    for batch in loader:
        lob    = batch["lob"].to(device)
        scalar = batch["scalar"].to(device)
        labels = batch["labels"].to(device)
        mask   = batch["mask"].to(device)

        preds = model(lob, scalar, mask)
        _, breakdown = criterion(preds, labels)

        for k, v in breakdown.items():
            totals[k] = totals.get(k, 0.0) + v
        n += 1

        # Collect last-step predictions (prediction at end of each window)
        pred_list.append(preds["returns"][:, -1, :].cpu().numpy())
        label_list.append(labels[:, -1, :].cpu().numpy())

    all_preds  = np.concatenate(pred_list,  axis=0)
    all_labels = np.concatenate(label_list, axis=0)
    return {k: v / max(n, 1) for k, v in totals.items()}, all_preds, all_labels


# ── Walk-forward driver ───────────────────────────────────────────────────────

def walk_forward_train(
    df,  # polars DataFrame
    cfg: TrainerConfig | None = None,
) -> list[dict]:
    """
    Run walk-forward training. Returns a list of fold results, each containing:
        fold        : int
        metrics     : eval loss breakdown
        predictions : (N, 3) return predictions (last-tick per window)
        labels      : (N, 5) ground truth
        model_state : state dict of best model for this fold
    """
    cfg = cfg or TrainerConfig()
    device = _device()
    print(f"Device: {device}")

    splits = split_walk_forward(df, n_folds=cfg.n_folds, train_frac=cfg.train_frac)
    ds_cfg = DatasetConfig(seq_len=cfg.seq_len)
    results = []

    for fold_idx, (train_df, test_df) in enumerate(splits):
        print(f"\n{'='*60}")
        print(f"Fold {fold_idx+1}/{len(splits)}  "
              f"train={len(train_df)} test={len(test_df)} ticks")

        train_loader, test_loader, _, _ = build_loaders(
            train_df, test_df, ds_cfg, batch_size=cfg.batch_size
        )
        if len(train_loader.dataset) == 0 or len(test_loader.dataset) == 0:
            print("  Skipping fold — not enough data for windows.")
            continue

        model = CryptoAlphaNet(
            d_spatial=cfg.d_spatial,
            d_temporal=cfg.d_temporal,
            n_lob_heads=cfg.n_lob_heads,
            n_lob_layers=cfg.n_lob_layers,
            n_temp_heads=cfg.n_temp_heads,
            n_temp_layers=cfg.n_temp_layers,
            dropout=cfg.dropout,
            seq_len=cfg.seq_len,
        ).to(device)

        if cfg.resume_state_dict is not None:
            model.load_state_dict(cfg.resume_state_dict, strict=False)

        criterion = MultiTaskLoss(
            w_return=cfg.w_return,
            w_direction=cfg.w_direction,
            w_risk=cfg.w_risk,
            w_tc=cfg.w_tc,
            tc_bps=cfg.tc_bps,
        )

        # Optional contrastive pre-training
        if cfg.pretrain and cfg.pretrain_epochs > 0:
            print(f"  Pre-training spatial encoder ({cfg.pretrain_epochs} epochs)…")
            pretrain_spatial(model, train_loader, cfg.pretrain_epochs, cfg.lr, device, cfg.adv_noise_std)

        optimizer = torch.optim.AdamW(
            model.parameters(), lr=cfg.lr, weight_decay=cfg.weight_decay
        )
        scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=cfg.epochs)

        best_val_loss = float("inf")
        best_state = None

        for epoch in range(cfg.epochs):
            train_metrics = train_epoch(
                model, train_loader, optimizer, criterion, device,
                grad_clip=cfg.grad_clip, noise_std=cfg.adv_noise_std,
            )
            scheduler.step()

            if (epoch + 1) % 5 == 0 or epoch == cfg.epochs - 1:
                val_metrics, preds, labels = eval_epoch(model, test_loader, criterion, device)
                val_loss = val_metrics["loss_total"]
                print(
                    f"  epoch {epoch+1:3d}/{cfg.epochs}  "
                    f"train={train_metrics['loss_total']:.4f}  "
                    f"val={val_loss:.4f}"
                )
                if val_loss < best_val_loss:
                    best_val_loss = val_loss
                    best_state = {k: v.cpu().clone() for k, v in model.state_dict().items()}

        # Final evaluation with best model
        if best_state is not None:
            model.load_state_dict(best_state)
        val_metrics, preds, labels = eval_epoch(model, test_loader, criterion, device)

        results.append({
            "fold":        fold_idx + 1,
            "metrics":     val_metrics,
            "predictions": preds,
            "labels":      labels,
            "model_state": best_state,
        })
        print(f"  Fold {fold_idx+1} best val loss: {best_val_loss:.4f}")

    return results
