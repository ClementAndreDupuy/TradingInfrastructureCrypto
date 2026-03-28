from __future__ import annotations

from contextlib import nullcontext
from dataclasses import dataclass
from typing import Callable

import numpy as np
import torch
import torch.nn as nn
from torch.utils.data import DataLoader

from ..data.dataset import DatasetConfig, build_loaders, split_train_validation, split_walk_forward
from .model import CryptoAlphaNet, MultiTaskLoss
from ..._config import model_cfg

_tcfg = model_cfg()["trainer"]


@dataclass
class TrainerConfig:
    d_spatial: int = _tcfg["d_spatial"]
    d_temporal: int = _tcfg["d_temporal"]
    n_lob_heads: int = _tcfg["n_lob_heads"]
    n_lob_layers: int = _tcfg["n_lob_layers"]
    n_temp_heads: int = _tcfg["n_temp_heads"]
    n_temp_layers: int = _tcfg["n_temp_layers"]
    dropout: float = _tcfg["dropout"]
    seq_len: int = _tcfg["seq_len"]
    epochs: int = _tcfg["epochs"]
    batch_size: int = _tcfg["batch_size"]
    lr: float = _tcfg["lr"]
    weight_decay: float = _tcfg["weight_decay"]
    grad_clip: float = _tcfg["grad_clip"]
    n_folds: int = _tcfg["n_folds"]
    train_frac: float = _tcfg["train_frac"]
    pretrain_epochs: int = _tcfg["pretrain_epochs"]
    pretrain: bool = _tcfg["pretrain"]
    w_return: float = _tcfg["w_return"]
    w_direction: float = _tcfg["w_direction"]
    w_risk: float = _tcfg["w_risk"]
    w_tc: float = _tcfg["w_tc"]
    selection_w_return: float | None = None
    selection_w_direction: float | None = None
    selection_w_risk: float | None = None
    selection_w_tc: float | None = None
    tc_bps: float = _tcfg["tc_bps"]
    adv_noise_std: float = _tcfg["adv_noise_std"]
    resume_state_dict: dict[str, torch.Tensor] | None = None
    lr_warmup_epochs: int = _tcfg["lr_warmup_epochs"]
    early_stop_patience: int = _tcfg["early_stop_patience"]
    log_every_epochs: int = _tcfg["log_every_epochs"]
    fold_seed_offset: int = _tcfg["fold_seed_offset"]
    event_callback: Callable[[dict[str, float | int]], None] | None = None
    use_amp: bool = _tcfg["use_amp"]
    validation_frac: float = _tcfg["validation_frac"]
    verbose: bool = True


def _device() -> torch.device:
    return torch.device("cuda" if torch.cuda.is_available() else "cpu")


def _maybe_autocast(device: torch.device, enabled: bool):
    if enabled and device.type == "cuda":
        return torch.autocast(device_type="cuda", dtype=torch.float16)
    return nullcontext()


def _augment_lob(lob: torch.Tensor, noise_std: float = 0.02) -> torch.Tensor:
    x = lob + torch.randn_like(lob) * noise_std
    if lob.shape[2] > 1:
        mask_level = torch.randint(0, lob.shape[2], (lob.shape[0], lob.shape[1]), device=lob.device)
        batch_idx = torch.arange(lob.shape[0], device=lob.device)[:, None]
        time_idx = torch.arange(lob.shape[1], device=lob.device)[None, :]
        x[batch_idx, time_idx, mask_level] = 0.0
    return x


def _class_weights_from_loader(loader: DataLoader, device: torch.device) -> tuple[torch.Tensor, torch.Tensor]:
    direction_counts = np.zeros(3, dtype=np.float64)
    risk_pos = 0.0
    risk_total = 0.0
    for batch in loader:
        labels = batch["labels"][:, :, 4].numpy().astype(np.int64)
        direction_counts += np.bincount(labels.reshape(-1), minlength=3)
        risk = batch["labels"][:, :, 5].numpy().astype(np.float64)
        risk_pos += float(risk.sum())
        risk_total += float(risk.size)
    direction_counts = np.clip(direction_counts, 1.0, None)
    direction_weights = direction_counts.sum() / (len(direction_counts) * direction_counts)
    risk_neg = max(risk_total - risk_pos, 1.0)
    risk_pos_weight = torch.tensor([risk_neg / max(risk_pos, 1.0)], dtype=torch.float32, device=device)
    return (
        torch.tensor(direction_weights, dtype=torch.float32, device=device),
        risk_pos_weight,
    )


def pretrain_spatial(
    model: CryptoAlphaNet,
    loader: DataLoader,
    epochs: int,
    lr: float,
    device: torch.device,
    noise_std: float = 0.02,
    verbose: bool = True,
) -> None:
    optimizer = torch.optim.Adam(model.spatial_enc.parameters(), lr=lr)
    model.spatial_enc.train()
    temperature = 0.07
    first_batch = next(iter(loader), None)
    if first_batch is not None:
        lob = first_batch["lob"].float()
        temporal_std = float(lob.std(dim=1).mean())
        if temporal_std < 0.0001:
            if verbose:
                print(f"  [pretrain] skipped — LOB near-constant across time (temporal std={temporal_std:.2e})")
            return
    for epoch in range(epochs):
        total_loss = 0.0
        n_batches = 0
        for batch in loader:
            lob = batch["lob"].to(device)
            lob_mean = lob.mean(dim=(0, 1), keepdim=True)
            lob_std = lob.std(dim=(0, 1), keepdim=True).clamp(min=1e-6)
            lob = (lob - lob_mean) / lob_std
            view1 = _augment_lob(lob, noise_std)
            view2 = _augment_lob(lob, noise_std)
            z1 = nn.functional.normalize(model.spatial_enc(view1).mean(dim=1), dim=-1)
            z2 = nn.functional.normalize(model.spatial_enc(view2).mean(dim=1), dim=-1)
            batch_size = z1.shape[0]
            z = torch.cat([z1, z2], dim=0)
            sim = torch.mm(z, z.T) / temperature
            sim.masked_fill_(torch.eye(2 * batch_size, device=device).bool(), float("-inf"))
            labels = torch.cat(
                [torch.arange(batch_size, 2 * batch_size, device=device), torch.arange(0, batch_size, device=device)]
            )
            loss = nn.functional.cross_entropy(sim, labels)
            optimizer.zero_grad()
            loss.backward()
            optimizer.step()
            total_loss += float(loss.item())
            n_batches += 1
        if n_batches > 0 and verbose:
            print(f"  [pretrain] epoch {epoch + 1}/{epochs}  loss={total_loss / n_batches:.4f}")


def train_epoch(
    model: CryptoAlphaNet,
    loader: DataLoader,
    optimizer: torch.optim.Optimizer,
    criterion: MultiTaskLoss,
    device: torch.device,
    grad_clip: float = 1.0,
    noise_std: float = 0.0,
    scaler: torch.cuda.amp.GradScaler | None = None,
    use_amp: bool = False,
) -> dict[str, float]:
    model.train()
    totals: dict[str, float] = {}
    n = 0
    for batch in loader:
        lob = batch["lob"].to(device)
        scalar = batch["scalar"].to(device)
        labels = batch["labels"].to(device)
        mask = batch["mask"].to(device)
        if noise_std > 0:
            lob = lob + torch.randn_like(lob) * noise_std
            scalar = scalar + torch.randn_like(scalar) * noise_std * 0.1
        optimizer.zero_grad(set_to_none=True)
        with _maybe_autocast(device, use_amp):
            preds = model(lob, scalar, mask)
            loss, breakdown = criterion(preds, labels)
        if scaler is not None and use_amp and device.type == "cuda":
            scaler.scale(loss).backward()
            scaler.unscale_(optimizer)
            nn.utils.clip_grad_norm_(model.parameters(), grad_clip)
            scaler.step(optimizer)
            scaler.update()
        else:
            loss.backward()
            nn.utils.clip_grad_norm_(model.parameters(), grad_clip)
            optimizer.step()
        for k, v in breakdown.items():
            totals[k] = totals.get(k, 0.0) + v
        n += 1
    return {k: v / max(n, 1) for (k, v) in totals.items()}


@torch.no_grad()
def eval_epoch(
    model: CryptoAlphaNet,
    loader: DataLoader,
    criterion: MultiTaskLoss,
    device: torch.device,
    use_amp: bool = False,
) -> tuple[dict[str, float], dict[str, np.ndarray], np.ndarray]:
    model.eval()
    totals: dict[str, float] = {}
    n = 0
    ret_pred_list: list[np.ndarray] = []
    dir_logit_list: list[np.ndarray] = []
    dir_prob_list: list[np.ndarray] = []
    risk_pred_list: list[np.ndarray] = []
    label_list: list[np.ndarray] = []
    for batch in loader:
        lob = batch["lob"].to(device)
        scalar = batch["scalar"].to(device)
        labels = batch["labels"].to(device)
        mask = batch["mask"].to(device)
        with _maybe_autocast(device, use_amp):
            preds = model(lob, scalar, mask)
            _, breakdown = criterion(preds, labels)
        for k, v in breakdown.items():
            totals[k] = totals.get(k, 0.0) + v
        n += 1
        ret_pred_list.append(preds["returns"][:, -1, :].float().cpu().numpy())
        dir_logits = preds["direction"][:, -1, :].float().cpu().numpy()
        dir_logit_list.append(dir_logits)
        dir_prob_list.append(torch.softmax(preds["direction"][:, -1, :].float(), dim=-1).cpu().numpy())
        risk_pred_list.append(preds["risk"][:, -1].float().cpu().numpy())
        label_list.append(labels[:, -1, :].float().cpu().numpy())
    all_outputs = {
        "returns": np.concatenate(ret_pred_list, axis=0),
        "direction_logits": np.concatenate(dir_logit_list, axis=0),
        "direction_probs": np.concatenate(dir_prob_list, axis=0),
        "risk": np.concatenate(risk_pred_list, axis=0),
    }
    all_labels = np.concatenate(label_list, axis=0)
    return ({k: v / max(n, 1) for (k, v) in totals.items()}, all_outputs, all_labels)


def selection_score(metrics: dict[str, float], cfg: TrainerConfig) -> float:
    w_return = cfg.selection_w_return if cfg.selection_w_return is not None else cfg.w_return
    w_direction = cfg.selection_w_direction if cfg.selection_w_direction is not None else cfg.w_direction
    w_risk = cfg.selection_w_risk if cfg.selection_w_risk is not None else cfg.w_risk
    w_tc = cfg.selection_w_tc if cfg.selection_w_tc is not None else cfg.w_tc
    return (
        w_return * metrics.get("loss_return", 0.0)
        + w_direction * metrics.get("loss_direction", 0.0)
        + w_risk * metrics.get("loss_risk", 0.0)
        + w_tc * metrics.get("loss_tc", 0.0)
    )


def _make_warmup_cosine_scheduler(
    optimizer: torch.optim.Optimizer, warmup_epochs: int, total_epochs: int
) -> torch.optim.lr_scheduler.LambdaLR:
    def _lr_lambda(epoch: int) -> float:
        if warmup_epochs > 0 and epoch < warmup_epochs:
            return float(epoch + 1) / float(warmup_epochs)
        progress = (epoch - warmup_epochs) / max(total_epochs - warmup_epochs, 1)
        return 0.5 * (1.0 + np.cos(np.pi * progress))

    return torch.optim.lr_scheduler.LambdaLR(optimizer, lr_lambda=_lr_lambda)


def walk_forward_train(df, cfg: TrainerConfig | None = None) -> list[dict]:
    cfg = cfg or TrainerConfig()
    device = _device()
    if cfg.verbose:
        print(f"Device: {device}")
    splits = split_walk_forward(df, n_folds=cfg.n_folds, train_frac=cfg.train_frac, min_samples=cfg.seq_len)
    ds_cfg = DatasetConfig(seq_len=cfg.seq_len)
    results = []
    for fold_idx, (train_df, test_df) in enumerate(splits):
        if cfg.verbose:
            print(f"\n{'=' * 60}")
            print(f"Fold {fold_idx + 1}/{len(splits)}  train={len(train_df)} test={len(test_df)} ticks")
        if cfg.fold_seed_offset > 0:
            seed = cfg.fold_seed_offset + fold_idx * 17
            torch.manual_seed(seed)
            np.random.seed(seed & 0xFFFFFFFF)
        fit_train_df, val_df = split_train_validation(
            train_df,
            validation_frac=cfg.validation_frac,
            min_samples=cfg.seq_len,
        )
        if len(val_df) == 0:
            if cfg.verbose:
                print("  Skipping fold — not enough data for an internal validation slice.")
            continue
        train_loader, val_loader, _, _ = build_loaders(fit_train_df, val_df, ds_cfg, batch_size=cfg.batch_size)
        _, test_loader, _, _ = build_loaders(train_df, test_df, ds_cfg, batch_size=cfg.batch_size)
        if len(train_loader.dataset) == 0 or len(val_loader.dataset) == 0 or len(test_loader.dataset) == 0:
            if cfg.verbose:
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
        direction_weights, risk_pos_weight = _class_weights_from_loader(train_loader, device)
        criterion = MultiTaskLoss(
            w_return=cfg.w_return,
            w_direction=cfg.w_direction,
            w_risk=cfg.w_risk,
            w_tc=cfg.w_tc,
            tc_bps=cfg.tc_bps,
            direction_class_weights=direction_weights,
            risk_pos_weight=risk_pos_weight,
        )
        if cfg.pretrain and cfg.pretrain_epochs > 0:
            if cfg.verbose:
                print(f"  Pre-training spatial encoder ({cfg.pretrain_epochs} epochs)…")
            pretrain_spatial(
                model,
                train_loader,
                cfg.pretrain_epochs,
                cfg.lr,
                device,
                cfg.adv_noise_std,
                verbose=cfg.verbose,
            )
        optimizer = torch.optim.AdamW(model.parameters(), lr=cfg.lr, weight_decay=cfg.weight_decay)
        scheduler = _make_warmup_cosine_scheduler(optimizer, warmup_epochs=cfg.lr_warmup_epochs, total_epochs=cfg.epochs)
        scaler = torch.amp.GradScaler('cuda',enabled=cfg.use_amp and device.type == "cuda")
        best_selection_loss = float("inf")
        best_state = None
        epochs_no_improve = 0
        best_metrics: dict[str, float] | None = None
        log_every = max(1, cfg.log_every_epochs)
        for epoch in range(cfg.epochs):
            train_metrics = train_epoch(
                model,
                train_loader,
                optimizer,
                criterion,
                device,
                grad_clip=cfg.grad_clip,
                noise_std=cfg.adv_noise_std,
                scaler=scaler,
                use_amp=cfg.use_amp,
            )
            scheduler.step()
            should_log = (epoch + 1) % log_every == 0 or epoch == cfg.epochs - 1
            if should_log:
                val_metrics, _, _ = eval_epoch(model, val_loader, criterion, device, use_amp=cfg.use_amp)
                current_selection_loss = selection_score(val_metrics, cfg)
                if cfg.verbose:
                    print(
                        f"  epoch {epoch + 1:3d}/{cfg.epochs}  train={train_metrics['loss_total']:.4f}  val={val_metrics['loss_total']:.4f}  sel={current_selection_loss:.4f}"
                    )
                if cfg.event_callback is not None:
                    cfg.event_callback({
                        "fold": fold_idx + 1,
                        "epoch": epoch + 1,
                        "total_epochs": cfg.epochs,
                        "train_loss": float(train_metrics["loss_total"]),
                        "val_loss": float(val_metrics["loss_total"]),
                        "selection_loss": float(current_selection_loss),
                    })
                if current_selection_loss < best_selection_loss:
                    best_selection_loss = current_selection_loss
                    best_state = {k: v.cpu().clone() for (k, v) in model.state_dict().items()}
                    best_metrics = dict(val_metrics)
                    epochs_no_improve = 0
                else:
                    epochs_no_improve += log_every
                    if cfg.early_stop_patience > 0 and epochs_no_improve >= cfg.early_stop_patience:
                        if cfg.verbose:
                            print(f"  Early stopping at epoch {epoch + 1} (no improvement for {epochs_no_improve} epochs)")
                        break
        if best_state is not None:
            model.load_state_dict(best_state)
        val_metrics, _, _ = eval_epoch(model, val_loader, criterion, device, use_amp=cfg.use_amp)
        test_metrics, outputs, labels = eval_epoch(model, test_loader, criterion, device, use_amp=cfg.use_amp)
        results.append(
            {
                "fold": fold_idx + 1,
                "metrics": test_metrics,
                "validation_metrics": val_metrics,
                "selection_score": selection_score(val_metrics, cfg),
                "best_selection_score": best_selection_loss,
                "best_metrics": best_metrics if best_metrics is not None else dict(val_metrics),
                "predictions": outputs["returns"],
                "direction_logits": outputs["direction_logits"],
                "direction_probs": outputs["direction_probs"],
                "risk_scores": outputs["risk"],
                "labels": labels,
                "model_state": best_state,
            }
        )
        if cfg.verbose:
            print(f"  Fold {fold_idx + 1} best selection score: {best_selection_loss:.4f}")
    return results
