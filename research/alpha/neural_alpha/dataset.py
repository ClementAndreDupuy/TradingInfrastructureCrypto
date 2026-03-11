"""
LOBDataset — sliding-window PyTorch Dataset over LOB snapshot sequences.

Reads from a Polars DataFrame produced by the data fetcher. Applies feature
engineering and builds (lob_tensor, scalar_features, labels) windows.
"""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np
import polars as pl
import torch
from torch.utils.data import Dataset

from .features import (
    compute_labels,
    compute_lob_tensor,
    compute_scalar_features,
    normalise_scalar,
)


@dataclass
class DatasetConfig:
    seq_len: int = 64        # number of ticks in each window
    stride:  int = 1         # step between windows (1 = maximum overlap)
    horizons: tuple[int, int, int] = (10, 100, 500)


class LOBDataset(Dataset):
    """
    Sliding-window dataset.

    Each sample is a dict:
        lob   : (seq_len, N_LEVELS, 4)  float32
        scalar: (seq_len, D_SCALAR)     float32
        labels: (seq_len, 5)            float32
        mask  : (seq_len,)              bool   True = valid (always True here)
    """

    def __init__(
        self,
        df: pl.DataFrame,
        cfg: DatasetConfig | None = None,
        scalar_mean: np.ndarray | None = None,
        scalar_std: np.ndarray | None = None,
    ) -> None:
        self.cfg = cfg or DatasetConfig()

        self.lob_arr    = compute_lob_tensor(df)          # (T, N_LEVELS, 4)
        raw_scalar      = compute_scalar_features(df)     # (T, D_SCALAR)
        self.labels_arr = compute_labels(df, self.cfg.horizons)  # (T, 5)

        self.scalar_arr, self.scalar_mean, self.scalar_std = normalise_scalar(
            raw_scalar, scalar_mean, scalar_std
        )

        T = len(self.lob_arr)
        S = self.cfg.seq_len
        # Start indices of valid windows (need S ticks of labels ahead too)
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
            "mask":   torch.zeros(self.cfg.seq_len, dtype=torch.bool),  # no padding
        }


def split_walk_forward(
    df: pl.DataFrame,
    n_folds: int = 4,
    train_frac: float = 0.75,
) -> list[tuple[pl.DataFrame, pl.DataFrame]]:
    """
    Walk-forward splits: each fold uses a rolling train window followed by a
    test window. No data leakage — test always comes after train.

    Returns list of (train_df, test_df) tuples.
    """
    T = len(df)
    fold_size = T // n_folds
    splits: list[tuple[pl.DataFrame, pl.DataFrame]] = []

    for i in range(n_folds):
        end_test  = (i + 1) * fold_size
        start_test = int(end_test - fold_size * (1 - train_frac))
        train_df  = df[:start_test]
        test_df   = df[start_test:end_test]
        if len(train_df) > 0 and len(test_df) > 0:
            splits.append((train_df, test_df))

    return splits


def build_loaders(
    train_df: pl.DataFrame,
    test_df: pl.DataFrame,
    cfg: DatasetConfig | None = None,
    batch_size: int = 32,
    num_workers: int = 0,
) -> tuple[torch.utils.data.DataLoader, torch.utils.data.DataLoader,
           np.ndarray, np.ndarray]:
    """
    Build train/test DataLoaders. Normalisation statistics are computed on
    training data only and reused for test.

    Returns:
        train_loader, test_loader, scalar_mean, scalar_std
    """
    from torch.utils.data import DataLoader

    cfg = cfg or DatasetConfig()
    train_ds = LOBDataset(train_df, cfg)
    test_ds  = LOBDataset(test_df, cfg, train_ds.scalar_mean, train_ds.scalar_std)

    train_loader = DataLoader(
        train_ds, batch_size=batch_size, shuffle=True,
        num_workers=num_workers, pin_memory=torch.cuda.is_available(),
    )
    test_loader = DataLoader(
        test_ds, batch_size=batch_size, shuffle=False,
        num_workers=num_workers, pin_memory=torch.cuda.is_available(),
    )
    return train_loader, test_loader, train_ds.scalar_mean, train_ds.scalar_std
