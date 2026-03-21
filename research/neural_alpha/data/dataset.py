from __future__ import annotations

from dataclasses import dataclass

import numpy as np
import polars as pl
import torch
from torch.utils.data import Dataset

from .features import compute_labels, compute_lob_tensor, compute_scalar_features


@dataclass
class DatasetConfig:
    seq_len: int = 64
    stride: int = 1
    horizons: tuple = (1, 10, 100, 500)


def rolling_normalise(
    x: np.ndarray,
    window: int = 500,
    history: np.ndarray | None = None,
) -> np.ndarray:
    """Rolling z-score normalisation with no future leakage.

    ``history`` lets the caller warm-start the rolling statistics with prior
    observations (for example the training split when normalising a holdout
    split). That keeps evaluation point-in-time safe while avoiding unstable
    first-batch scaling on a fresh test slice.
    """
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
    """Sliding-window dataset.

    Each sample is a dict:
        lob   : (seq_len, N_LEVELS, 4)  float32
        scalar: (seq_len, D_SCALAR)     float32
        labels: (seq_len, 6)            float32
        mask  : (seq_len,)              bool
    """

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
    n_folds: int = 4,
    train_frac: float = 0.75,
    min_samples: int = 1,
) -> list[tuple[pl.DataFrame, pl.DataFrame]]:
    """Walk-forward splits with no data leakage — test always follows train.

    ``min_samples`` is the minimum rows required in both train and test slices;
    pass ``seq_len`` to filter out folds too short for a sliding window."""
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


def build_loaders(
    train_df: pl.DataFrame,
    test_df: pl.DataFrame,
    cfg: DatasetConfig | None = None,
    batch_size: int = 32,
    num_workers: int = 0,
) -> tuple[torch.utils.data.DataLoader, torch.utils.data.DataLoader,
           np.ndarray | None, np.ndarray | None]:
    """Build train/test DataLoaders with per-dataset rolling normalisation."""
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
