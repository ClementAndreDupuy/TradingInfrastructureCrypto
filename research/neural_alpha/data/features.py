from __future__ import annotations

import numpy as np
import polars as pl

N_LEVELS = 5
BASE_SCALAR_FEATURES = 16
D_SCALAR = BASE_SCALAR_FEATURES + N_LEVELS  # queue imbalance contributes one feature per level


def _safe_col(df: pl.DataFrame, name: str, default: float = 0.0) -> np.ndarray:
    if name in df.columns:
        return df[name].to_numpy(allow_copy=True).astype(np.float64)
    return np.full(len(df), default, dtype=np.float64)


def _extract_levels(df: pl.DataFrame) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """Returns bid_prices, bid_sizes, ask_prices, ask_sizes each shaped (T, N_LEVELS).
    Handles both list-column and flat-column schemas."""
    T = len(df)

    def _list_col(name: str, fallback_fmt: str) -> np.ndarray:
        if name in df.columns and df[name].dtype == pl.List(pl.Float64):
            arr = np.array(df[name].to_list(), dtype=np.float64)
            if arr.ndim == 1:
                arr = arr[:, None]
            return arr[:, :N_LEVELS]
        cols = []
        for i in range(1, N_LEVELS + 1):
            col = fallback_fmt.format(i)
            if col in df.columns:
                cols.append(df[col].to_numpy(allow_copy=True).astype(np.float64))
            else:
                cols.append(np.zeros(T, dtype=np.float64))
        return np.stack(cols, axis=1)

    bp = _list_col("bid_prices", "bid_price_{}")
    bs = _list_col("bid_sizes", "bid_size_{}")
    ap = _list_col("ask_prices", "ask_price_{}")
    as_ = _list_col("ask_sizes", "ask_size_{}")

    def _pad_to_levels(arr: np.ndarray) -> np.ndarray:
        if arr.shape[1] < N_LEVELS:
            return np.pad(arr, ((0, 0), (0, N_LEVELS - arr.shape[1])))
        return arr

    return _pad_to_levels(bp), _pad_to_levels(bs), _pad_to_levels(ap), _pad_to_levels(as_)


def compute_lob_tensor(df: pl.DataFrame) -> np.ndarray:
    """Build the (T, N_LEVELS, 4) LOB tensor.
    Each level: [normalised_bid_price, bid_size_share, normalised_ask_price, ask_size_share]
    Prices are expressed as tick offsets from mid-price; sizes as fractional depth share."""
    bp, bs, ap, as_ = _extract_levels(df)

    best_bid = bp[:, 0]
    best_ask = ap[:, 0]
    mid = (best_bid + best_ask) / 2.0
    spread = np.where(best_ask - best_bid > 0, best_ask - best_bid, 1.0)

    bid_p_norm = (bp - mid[:, None]) / spread[:, None]
    ask_p_norm = (ap - mid[:, None]) / spread[:, None]

    bid_s_norm = bs / bs.sum(axis=1, keepdims=True).clip(1e-9)
    ask_s_norm = as_ / as_.sum(axis=1, keepdims=True).clip(1e-9)

    return np.stack([bid_p_norm, bid_s_norm, ask_p_norm, ask_s_norm], axis=2).astype(np.float32)


def _lag_diff(arr: np.ndarray, lag: int) -> np.ndarray:
    """arr[t] - arr[t-lag], with boundary filled from arr[0]."""
    if lag <= 0:
        return np.zeros_like(arr)
    if lag >= len(arr):
        return arr - arr[0]
    return arr - np.concatenate([np.full(lag, arr[0]), arr[:-lag]])


def _rolling_std(x: np.ndarray, window: int) -> np.ndarray:
    """Vectorized rolling std using cumsum trick. O(T) instead of O(T*window)."""
    T = len(x)
    if T == 0:
        return np.zeros(0, dtype=np.float64)

    cum = np.concatenate([[0.0], np.cumsum(x)])
    cum2 = np.concatenate([[0.0], np.cumsum(x**2)])

    end = np.arange(1, T + 1)
    start = np.maximum(0, end - max(1, window))

    s1 = cum[end] - cum[start]
    s2 = cum2[end] - cum2[start]
    n = (end - start).astype(np.float64)
    return np.sqrt((s2 / n - (s1 / n) ** 2).clip(0))


def compute_scalar_features(df: pl.DataFrame) -> np.ndarray:
    """
    Return (T, D_SCALAR) array of derived features:
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
        13..(13+N_LEVELS-1) qi_i — per-level queue imbalance for each level i in [1, N_LEVELS]
        D_SCALAR-3           vol_60             — rolling 60-tick std of mid log return
        D_SCALAR-2           vol_200            — rolling 200-tick std of mid log return
        D_SCALAR-1           vol_ratio_5_60     — vol_5 / (vol_60 + 1e-8)
    """
    bp, bs, ap, as_ = _extract_levels(df)

    best_bid = bp[:, 0]
    best_ask = ap[:, 0]
    mid = np.where((best_bid + best_ask) / 2.0 > 0, (best_bid + best_ask) / 2.0, np.nan)

    spread_bps = np.where(mid > 0, (best_ask - best_bid) / mid * 1e4, 0.0)

    denom = bs[:, 0] + as_[:, 0]
    denom = np.where(denom > 0, denom, 1.0)
    microprice = (best_bid * as_[:, 0] + best_ask * bs[:, 0]) / denom

    depth_bid = bs.sum(axis=1)
    depth_ask = as_.sum(axis=1)
    total_depth = depth_bid + depth_ask
    obi = np.where(total_depth > 0, depth_bid / total_depth, 0.5)

    ofi_1 = _lag_diff(depth_bid, 1) - _lag_diff(depth_ask, 1)
    ofi_5 = _lag_diff(depth_bid, 5) - _lag_diff(depth_ask, 5)
    ofi_10 = _lag_diff(depth_bid, 10) - _lag_diff(depth_ask, 10)
    ofi_20 = _lag_diff(depth_bid, 20) - _lag_diff(depth_ask, 20)

    log_mid = np.log(np.where(mid > 0, mid, np.nan))
    log_ret = np.nan_to_num(np.diff(log_mid, prepend=log_mid[0]), nan=0.0, posinf=0.0, neginf=0.0)

    vol_5 = _rolling_std(log_ret, 5)
    vol_20 = _rolling_std(log_ret, 20)
    vol_60 = _rolling_std(log_ret, 60)
    vol_200 = _rolling_std(log_ret, 200)

    qi = (bs - as_) / (bs + as_ + 1e-8)

    base_features = [
        log_ret, spread_bps, microprice - mid, obi,
        np.log1p(depth_bid), np.log1p(depth_ask),
        ofi_1, log_ret * total_depth,
        vol_5, vol_20, ofi_5, ofi_10, ofi_20,
        vol_60, vol_200, vol_5 / (vol_60 + 1e-8),
    ]
    features = np.stack(
        base_features[:13] + [qi[:, i] for i in range(N_LEVELS)] + base_features[13:],
        axis=1,
    )
    return np.nan_to_num(features, nan=0.0, posinf=0.0, neginf=0.0).astype(np.float32)


def compute_labels(df: pl.DataFrame, horizons: tuple[int, ...] = (1, 10, 100, 500)) -> np.ndarray:
    """
    Compute multi-horizon forward log-returns.

    Returns (T, 6) array:
        col 0-3 : log returns at each horizon (clipped at ±5 bps)
        col 4   : direction at mid horizon index 2 (0=down, 1=flat, 2=up)
        col 5   : adverse selection proxy (0/1) — price reversion within 10 ticks
    """
    bp, _, ap, _ = _extract_levels(df)
    mid = np.where((bp[:, 0] + ap[:, 0]) / 2.0 > 0, (bp[:, 0] + ap[:, 0]) / 2.0, np.nan)
    log_mid = np.log(np.where(mid > 0, mid, 1.0))

    T = len(df)
    returns = np.zeros((T, len(horizons)), dtype=np.float32)
    for i, h in enumerate(horizons):
        h = min(h, T - 1)
        fwd = np.zeros(T, dtype=np.float32)
        if h > 0 and T > h:
            fwd[: T - h] = log_mid[h:] - log_mid[: T - h]
        returns[:, i] = np.clip(fwd, -0.0005, 0.0005)

    # Direction based on mid horizon (index 2 = 100-tick)
    flat_thresh = 2e-5  # ~0.2 bps
    mid_ret = returns[:, 2]
    direction = np.where(mid_ret > flat_thresh, 2, np.where(mid_ret < -flat_thresh, 0, 1)).astype(
        np.float32
    )

    # Adverse selection: infer fill direction from immediate move, mark adverse if price
    # reverts against that direction within the next 10 ticks.
    reversion_h = 10
    fwd_ret_1 = returns[:, 0]
    adv_sel = np.zeros(T, dtype=np.float32)
    for t in range(T - reversion_h):
        r1 = float(fwd_ret_1[t])
        if abs(r1) <= 1e-6:
            continue
        fill_dir = 1.0 if r1 > 0 else -1.0
        path = log_mid[t + 1 : t + 1 + reversion_h] - log_mid[t]
        if path.size == 0:
            continue
        reversion = np.min(path) if fill_dir > 0 else -np.max(path)
        if reversion < -2e-5:
            adv_sel[t] = 1.0

    return np.concatenate([returns, direction[:, None], adv_sel[:, None]], axis=1).astype(np.float32)


def normalise_scalar(
    features: np.ndarray, mean: np.ndarray | None = None, std: np.ndarray | None = None
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Z-score normalise scalar features. Returns (normalised, mean, std)."""
    if mean is None:
        mean = features.mean(axis=0)
    if std is None:
        std = features.std(axis=0) + 1e-8
    return (features - mean) / std, mean, std
