"""
Feature engineering from L5 order book snapshots and tick data.

Input schema (Polars DataFrame):
    timestamp_ns  : int64
    exchange      : str
    bid_prices    : list[float]  — top N bid levels, best first
    bid_sizes     : list[float]
    ask_prices    : list[float]  — top N ask levels, best first
    ask_sizes     : list[float]

Outputs two arrays:
    lob_tensor  : (T, N_LEVELS, 4)  — per-level [bid_p, bid_s, ask_p, ask_s] normalised
    scalar_feat : (T, D_SCALAR)     — midprice-derived features
"""
from __future__ import annotations

import numpy as np
import polars as pl

N_LEVELS = 5
D_SCALAR = 21  # number of scalar features per tick


def _safe_col(df: pl.DataFrame, name: str, default: float = 0.0) -> np.ndarray:
    if name in df.columns:
        return df[name].to_numpy(allow_copy=True).astype(np.float64)
    return np.full(len(df), default, dtype=np.float64)


def _extract_levels(df: pl.DataFrame) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """
    Returns bid_prices, bid_sizes, ask_prices, ask_sizes each shaped (T, N_LEVELS).
    Handles both list-column and flat-column schemas.
    """
    T = len(df)

    def _list_col(name: str, fallback_fmt: str) -> np.ndarray:
        if name in df.columns and df[name].dtype == pl.List(pl.Float64):
            arr = np.array(df[name].to_list(), dtype=np.float64)
            if arr.ndim == 1:
                arr = arr[:, None]
            return arr[:, :N_LEVELS]
        # flat columns: bid_price_1 … bid_price_N
        cols = []
        for i in range(1, N_LEVELS + 1):
            col = fallback_fmt.format(i)
            if col in df.columns:
                cols.append(df[col].to_numpy(allow_copy=True).astype(np.float64))
            else:
                cols.append(np.zeros(T, dtype=np.float64))
        return np.stack(cols, axis=1)  # (T, N_LEVELS)

    bp = _list_col("bid_prices", "bid_price_{}")
    bs = _list_col("bid_sizes",  "bid_size_{}")
    ap = _list_col("ask_prices", "ask_price_{}")
    as_ = _list_col("ask_sizes", "ask_size_{}")

    # Pad or truncate to N_LEVELS
    for arr in (bp, bs, ap, as_):
        if arr.shape[1] < N_LEVELS:
            pad = N_LEVELS - arr.shape[1]
            arr = np.pad(arr, ((0, 0), (0, pad)))

    return bp, bs, ap, as_


def compute_lob_tensor(df: pl.DataFrame) -> np.ndarray:
    """
    Build the (T, N_LEVELS, 4) LOB tensor.

    Each level: [normalised_bid_price, bid_size_share, normalised_ask_price, ask_size_share]

    Prices are expressed as tick offsets from mid-price (dimensionless).
    Sizes are fractional share of total visible depth on each side.
    """
    bp, bs, ap, as_ = _extract_levels(df)

    best_bid = bp[:, 0]
    best_ask = ap[:, 0]
    mid = (best_bid + best_ask) / 2.0
    spread = np.where(mid > 0, best_ask - best_bid, 1.0)

    # Normalise prices as distance from mid in units of spread
    bid_p_norm = (bp - mid[:, None]) / spread[:, None]   # negative for bids
    ask_p_norm = (ap - mid[:, None]) / spread[:, None]   # positive for asks

    # Normalise sizes as fraction of total depth
    total_bid = bs.sum(axis=1, keepdims=True).clip(1e-9)
    total_ask = as_.sum(axis=1, keepdims=True).clip(1e-9)
    bid_s_norm = bs / total_bid
    ask_s_norm = as_ / total_ask

    # Stack into (T, N_LEVELS, 4)
    lob = np.stack([bid_p_norm, bid_s_norm, ask_p_norm, ask_s_norm], axis=2)
    return lob.astype(np.float32)


def _lag_diff(arr: np.ndarray, lag: int) -> np.ndarray:
    """arr[t] - arr[t-lag], with boundary filled from arr[0]."""
    return arr - np.concatenate([np.full(lag, arr[0]), arr[:-lag]])


def _rolling_std(x: np.ndarray, window: int) -> np.ndarray:
    """Vectorized rolling std using cumsum trick. O(T) instead of O(T*window)."""
    cum  = np.cumsum(x)
    cum2 = np.cumsum(x ** 2)
    n    = np.minimum(np.arange(1, len(x) + 1), window).astype(np.float64)
    s1   = cum - np.concatenate([np.zeros(window), cum[:-window]])
    s2   = cum2 - np.concatenate([np.zeros(window), cum2[:-window]])
    var  = (s2 / n - (s1 / n) ** 2).clip(0)
    return np.sqrt(var)


def compute_scalar_features(df: pl.DataFrame) -> np.ndarray:
    """
    Return (T, D_SCALAR=21) array of derived features:
        0  mid_return_lag1    — tick-to-tick midprice log return
        1  spread_bps         — bid-ask spread in bps
        2  microprice         — size-weighted price deviation from mid
        3  obi                — order-book imbalance (bid_depth / total_depth)
        4  depth_bid          — log total bid depth
        5  depth_ask          — log total ask depth
        6  ofi_1              — order-flow imbalance lag-1
        7  signed_flow        — signed trade flow proxy (mid_return * total_depth)
        8  vol_5              — rolling 5-tick std of mid log return
        9  vol_20             — rolling 20-tick std of mid log return
        10 ofi_5              — order-flow imbalance lag-5
        11 ofi_10             — order-flow imbalance lag-10
        12 ofi_20             — order-flow imbalance lag-20
        13 qi_1               — per-level queue imbalance level 1
        14 qi_2               — per-level queue imbalance level 2
        15 qi_3               — per-level queue imbalance level 3
        16 qi_4               — per-level queue imbalance level 4
        17 qi_5               — per-level queue imbalance level 5
        18 vol_60             — rolling 60-tick std of mid log return
        19 vol_200            — rolling 200-tick std of mid log return
        20 vol_ratio_5_60     — vol_5 / (vol_60 + 1e-8)
    """
    bp, bs, ap, as_ = _extract_levels(df)

    best_bid = bp[:, 0]
    best_ask = ap[:, 0]
    mid = (best_bid + best_ask) / 2.0
    mid = np.where(mid > 0, mid, np.nan)

    spread_bps = np.where(mid > 0, (best_ask - best_bid) / mid * 1e4, 0.0)

    # Microprice: size-weighted
    total_bid_top = bs[:, 0]
    total_ask_top = as_[:, 0]
    denom = total_bid_top + total_ask_top
    denom = np.where(denom > 0, denom, 1.0)
    microprice = (best_bid * total_ask_top + best_ask * total_bid_top) / denom

    # OBI
    depth_bid = bs.sum(axis=1)
    depth_ask = as_.sum(axis=1)
    total_depth = depth_bid + depth_ask
    obi = np.where(total_depth > 0, depth_bid / total_depth, 0.5)

    log_depth_bid = np.log1p(depth_bid)
    log_depth_ask = np.log1p(depth_ask)

    # OFI: delta(bid_depth) - delta(ask_depth) at lags 1, 5, 10, 20
    ofi_1  = _lag_diff(depth_bid, 1)  - _lag_diff(depth_ask, 1)
    ofi_5  = _lag_diff(depth_bid, 5)  - _lag_diff(depth_ask, 5)
    ofi_10 = _lag_diff(depth_bid, 10) - _lag_diff(depth_ask, 10)
    ofi_20 = _lag_diff(depth_bid, 20) - _lag_diff(depth_ask, 20)

    # Log mid-price return
    log_mid = np.log(np.where(mid > 0, mid, np.nan))
    log_ret = np.diff(log_mid, prepend=log_mid[0])
    log_ret = np.nan_to_num(log_ret, nan=0.0, posinf=0.0, neginf=0.0)

    signed_flow = log_ret * total_depth

    # Rolling volatility (vectorized)
    vol_5   = _rolling_std(log_ret, 5)
    vol_20  = _rolling_std(log_ret, 20)
    vol_60  = _rolling_std(log_ret, 60)
    vol_200 = _rolling_std(log_ret, 200)
    vol_ratio_5_60 = vol_5 / (vol_60 + 1e-8)

    # Per-level queue imbalance: (bid_size_i - ask_size_i) / (bid_size_i + ask_size_i + 1e-8)
    qi = (bs - as_) / (bs + as_ + 1e-8)  # (T, N_LEVELS)

    features = np.stack([
        log_ret, spread_bps, microprice - mid,
        obi, log_depth_bid, log_depth_ask,
        ofi_1, signed_flow, vol_5, vol_20,
        ofi_5, ofi_10, ofi_20,
        qi[:, 0], qi[:, 1], qi[:, 2], qi[:, 3], qi[:, 4],
        vol_60, vol_200, vol_ratio_5_60,
    ], axis=1)

    features = np.nan_to_num(features, nan=0.0, posinf=0.0, neginf=0.0)
    return features.astype(np.float32)


def compute_labels(df: pl.DataFrame, horizons: tuple[int, ...] = (1, 10, 100, 500)) -> np.ndarray:
    """
    Compute multi-horizon forward log-returns.

    Args:
        horizons: tick offsets (1-tick, short, mid, long)

    Returns:
        (T, 6) array:
            col 0-3 : log returns at each horizon (clipped at ±5 bps)
            col 4   : direction at mid horizon index 2 (0=down, 1=flat, 2=up)
            col 5   : adverse selection proxy (0/1) — price reversion within 10 ticks
    """
    bp, _, ap, _ = _extract_levels(df)
    best_bid = bp[:, 0]
    best_ask = ap[:, 0]
    mid = (best_bid + best_ask) / 2.0
    mid = np.where(mid > 0, mid, np.nan)
    log_mid = np.log(np.where(mid > 0, mid, 1.0))

    T = len(df)
    n_horizons = len(horizons)
    returns = np.zeros((T, n_horizons), dtype=np.float32)
    for i, h in enumerate(horizons):
        h = min(h, T - 1)
        fwd = np.zeros(T, dtype=np.float32)
        if h > 0 and T > h:
            fwd[: T - h] = log_mid[h:] - log_mid[: T - h]
        returns[:, i] = np.clip(fwd, -0.0005, 0.0005)

    # Direction based on mid horizon (index 2 = 100-tick)
    flat_thresh = 2e-5  # ~0.2 bps
    mid_ret = returns[:, 2]
    direction = np.where(mid_ret > flat_thresh, 2,
                np.where(mid_ret < -flat_thresh, 0, 1)).astype(np.float32)

    # Adverse selection using fill-reversion model:
    # infer fill direction from immediate move, then mark adverse if
    # price reverts against that direction within next N ticks.
    reversion_h = 10
    fwd_ret_1 = returns[:, 0]
    adv_sel = np.zeros(T, dtype=np.float32)
    for t in range(T - reversion_h):
        r1 = float(fwd_ret_1[t])
        if abs(r1) <= 1e-6:
            continue
        fill_dir = 1.0 if r1 > 0 else -1.0
        path = log_mid[t + 1: t + 1 + reversion_h] - log_mid[t]
        if path.size == 0:
            continue
        if fill_dir > 0:
            reversion = np.min(path)
        else:
            reversion = -np.max(path)
        if reversion < -2e-5:
            adv_sel[t] = 1.0

    labels = np.concatenate([returns, direction[:, None], adv_sel[:, None]], axis=1)
    return labels.astype(np.float32)


def normalise_scalar(features: np.ndarray, mean: np.ndarray | None = None,
                     std: np.ndarray | None = None) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Z-score normalise scalar features. Returns (normalised, mean, std)."""
    if mean is None:
        mean = features.mean(axis=0)
    if std is None:
        std = features.std(axis=0) + 1e-8
    return (features - mean) / std, mean, std
