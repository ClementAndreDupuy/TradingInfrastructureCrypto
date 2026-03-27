from __future__ import annotations

from dataclasses import dataclass

import numpy as np
import polars as pl
import torch
from torch.utils.data import Dataset

from .features import compute_labels, compute_lob_tensor, compute_scalar_features
from ..._config import model_cfg

_mcfg = model_cfg()
_dscfg = _mcfg["dataset"]


@dataclass
class DatasetConfig:
    seq_len: int = _dscfg["seq_len"]
    stride: int = _dscfg["stride"]
    horizons: tuple = tuple(_dscfg["horizons"])


def rolling_normalise(
    x: np.ndarray,
    window: int = _dscfg["rolling_normalise_window"],
    history: np.ndarray | None = None,
) -> np.ndarray:
    T, D = x.shape
    if T == 0:
        return x.astype(np.float32)

    hist = np.empty((0, D), dtype=np.float64) if history is None else np.asarray(history, dtype=np.float64)
    if hist.ndim == 1:
        hist = hist[:, None]
    if hist.size and hist.shape[1] != D:
        raise ValueError(f"history feature dimension {hist.shape[1]} does not match x dimension {D}")

    combined = np.vstack([hist[-window:], x.astype(np.float64, copy=False)])
    total = len(combined)
    cum = np.vstack([np.zeros((1, D), dtype=np.float64), np.cumsum(combined, axis=0)])
    cum2 = np.vstack([np.zeros((1, D), dtype=np.float64), np.cumsum(combined ** 2, axis=0)])

    current_idx = np.arange(len(hist), total)
    end = current_idx + 1
    start = np.maximum(0, end - window)

    s1 = cum[end] - cum[start]
    s2 = cum2[end] - cum2[start]
    n = (end - start)[:, None].astype(np.float64)
    mean = s1 / n
    var = (s2 / n - mean ** 2).clip(0)
    return ((combined[current_idx] - mean) / (np.sqrt(var) + 1e-8)).astype(np.float32)


class LOBDataset(Dataset):

    def __init__(
        self,
        df: pl.DataFrame,
        cfg: DatasetConfig | None = None,
        scalar_mean: np.ndarray | None = None,
        scalar_std: np.ndarray | None = None,
    ) -> None:
        self.cfg = cfg or DatasetConfig()

        self.lob_arr = compute_lob_tensor(df)
        raw_scalar = compute_scalar_features(df)
        self.raw_scalar_arr = raw_scalar.astype(np.float32)
        self.labels_arr = compute_labels(df, self.cfg.horizons)
        self.scalar_arr = rolling_normalise(self.raw_scalar_arr, history=scalar_mean)
        self.scalar_mean: np.ndarray | None = scalar_mean
        self.scalar_std: np.ndarray | None = scalar_std

        T = len(self.lob_arr)
        S = self.cfg.seq_len
        self.indices = list(range(0, T - S + 1, self.cfg.stride))

    def __len__(self) -> int:
        return len(self.indices)

    def __getitem__(self, idx: int) -> dict[str, torch.Tensor]:
        start = self.indices[idx]
        end   = start + self.cfg.seq_len

        return {
            "lob":    torch.from_numpy(self.lob_arr[start:end]),
            "scalar": torch.from_numpy(self.scalar_arr[start:end]),
            "labels": torch.from_numpy(self.labels_arr[start:end]),
            "mask": torch.zeros(self.cfg.seq_len, dtype=torch.bool),
        }


def split_walk_forward(
    df: pl.DataFrame,
    n_folds: int = _dscfg["walk_forward_folds"],
    train_frac: float = _dscfg["walk_forward_train_frac"],
    min_samples: int = 1,
) -> list[tuple[pl.DataFrame, pl.DataFrame]]:
    T = len(df)
    fold_size = T // n_folds
    splits: list[tuple[pl.DataFrame, pl.DataFrame]] = []

    for i in range(n_folds):
        end_test  = (i + 1) * fold_size
        start_test = int(end_test - fold_size * (1 - train_frac))
        train_df  = df[:start_test]
        test_df   = df[start_test:end_test]
        if len(train_df) >= min_samples and len(test_df) >= min_samples:
            splits.append((train_df, test_df))

    return splits


def split_train_validation(
    train_df: pl.DataFrame,
    validation_frac: float = _dscfg["validation_frac"],
    min_samples: int = 1,
) -> tuple[pl.DataFrame, pl.DataFrame]:
    if len(train_df) == 0:
        return (train_df, train_df.clear())
    if not 0.0 < validation_frac < 1.0:
        raise ValueError("validation_frac must be between 0 and 1")

    val_size = max(min_samples, int(round(len(train_df) * validation_frac)))
    if len(train_df) - val_size < min_samples:
        return (train_df, train_df.clear())

    split_idx = len(train_df) - val_size
    return (train_df[:split_idx], train_df[split_idx:])


def build_loaders(
    train_df: pl.DataFrame,
    test_df: pl.DataFrame,
    cfg: DatasetConfig | None = None,
    batch_size: int = 32,
    num_workers: int = 0,
) -> tuple[torch.utils.data.DataLoader, torch.utils.data.DataLoader,
           np.ndarray | None, np.ndarray | None]:
    from torch.utils.data import DataLoader

    cfg = cfg or DatasetConfig()
    train_ds = LOBDataset(train_df, cfg)
    test_ds = LOBDataset(test_df, cfg, scalar_mean=train_ds.raw_scalar_arr[-cfg.seq_len:])

    train_loader = DataLoader(
        train_ds, batch_size=batch_size, shuffle=True,
        num_workers=num_workers, pin_memory=torch.cuda.is_available(),
    )
    test_loader = DataLoader(
        test_ds, batch_size=batch_size, shuffle=False,
        num_workers=num_workers, pin_memory=torch.cuda.is_available(),
    )
    return train_loader, test_loader, None, None
