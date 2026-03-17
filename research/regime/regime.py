from __future__ import annotations

import json
import mmap
import struct
import time
from dataclasses import asdict, dataclass
from pathlib import Path

import numpy as np
import polars as pl


@dataclass
class RegimeConfig:
    n_regimes: int = 4
    max_iter: int = 50
    tol: float = 1e-4
    random_seed: int = 7


@dataclass
class RegimeArtifact:
    version: str
    n_regimes: int
    feature_columns: list[str]
    regime_names: list[str]
    centers: list[list[float]]
    scales: list[float]


def _feature_frame(df: pl.DataFrame) -> pl.DataFrame:
    base = df.with_columns(
        [
            ((pl.col("best_bid") + pl.col("best_ask")) / 2.0).alias("mid"),
            (pl.col("best_ask") - pl.col("best_bid")).clip(lower_bound=1e-8).alias("spread"),
            (pl.col("bid_size_1") + pl.col("ask_size_1")).alias("top_depth"),
            (
                (pl.col("bid_size_1") - pl.col("ask_size_1"))
                / (pl.col("bid_size_1") + pl.col("ask_size_1") + 1e-8)
            ).alias("queue_imbalance"),
        ]
    )
    feat = base.with_columns(
        [
            pl.col("mid").log().diff().fill_null(0.0).alias("log_ret_1"),
        ]
    ).with_columns(
        [
            pl.col("log_ret_1").rolling_std(window_size=20).fill_null(0.0).alias("vol_20"),
            pl.col("spread").rolling_mean(window_size=20).fill_null(strategy="backward").fill_null(0.0).alias("spread_20"),
            pl.col("top_depth").rolling_mean(window_size=20).fill_null(strategy="backward").fill_null(0.0).alias("depth_20"),
        ]
    )

    return feat.select(["vol_20", "spread_20", "depth_20", "queue_imbalance", "log_ret_1"])


def _load_ipc_lob_frame(ipc_dir: str) -> pl.DataFrame:
    root = Path(ipc_dir)
    if not root.exists() or not root.is_dir():
        raise FileNotFoundError(f"IPC directory not found: {ipc_dir}")

    candidates = sorted(list(root.glob("*.parquet")) + list(root.glob("*.csv")))
    if not candidates:
        raise FileNotFoundError(f"No *.parquet or *.csv files found under {ipc_dir}")

    frames: list[pl.DataFrame] = []
    required = {"timestamp_ns", "best_bid", "best_ask", "bid_size_1", "ask_size_1"}
    for path in candidates:
        if path.suffix == ".parquet":
            frame = pl.read_parquet(path)
        else:
            frame = pl.read_csv(path)

        if required.issubset(set(frame.columns)):
            frames.append(frame)

    if not frames:
        raise ValueError(f"No IPC files in {ipc_dir} had required LOB columns")

    lob = pl.concat(frames, how="vertical_relaxed").sort("timestamp_ns")

    null_cols = [
        c for c in ("best_bid", "best_ask", "bid_size_1", "ask_size_1")
        if lob[c].is_null().any()
    ]
    if null_cols:
        raise ValueError(f"Null values found in LOB columns: {null_cols}")

    return lob


def _kmeans(x: np.ndarray, cfg: RegimeConfig) -> tuple[np.ndarray, np.ndarray]:
    rng = np.random.default_rng(cfg.random_seed)
    centers = x[rng.choice(len(x), size=cfg.n_regimes, replace=False)].copy()
    labels = np.zeros(len(x), dtype=np.int32)

    for _ in range(cfg.max_iter):
        dist = ((x[:, None, :] - centers[None, :, :]) ** 2).sum(axis=2)
        new_labels = np.argmin(dist, axis=1).astype(np.int32)
        new_centers = centers.copy()
        for k in range(cfg.n_regimes):
            mask = new_labels == k
            if np.any(mask):
                new_centers[k] = x[mask].mean(axis=0)
        shift = float(np.linalg.norm(new_centers - centers))
        centers = new_centers
        labels = new_labels
        if shift < cfg.tol:
            break

    return labels, centers


def _semantic_regime_names(raw_centers: np.ndarray) -> list[str]:
    n = raw_centers.shape[0]
    names = [f"regime_{i}" for i in range(n)]

    def _assign_first_free(ranked: np.ndarray, label: str) -> None:
        for idx in reversed(ranked):
            if names[idx].startswith("regime_"):
                names[idx] = label
                return

    vol_rank = np.argsort(raw_centers[:, 0])

    # "calm" always wins the lowest-volatility cluster
    names[vol_rank[0]] = "calm"
    # Remaining labels use next-best free cluster to prevent collisions.
    # Collision example: if highest-vol == highest-spread cluster, the old
    # code would overwrite "shock" with "illiquid" leaving no shock label and
    # breaking the C++ market-maker's shock-gating threshold.
    _assign_first_free(vol_rank, "shock")
    if n >= 3:
        _assign_first_free(np.argsort(raw_centers[:, 1]), "illiquid")
    if n >= 4:
        _assign_first_free(np.argsort(raw_centers[:, 2]), "trending")

    return names


def train_regime_model_from_ipc(
    ipc_dir: str,
    cfg: RegimeConfig,
) -> tuple[RegimeArtifact, dict[str, float]]:
    if cfg.n_regimes not in {3, 4}:
        raise ValueError("R2 regime model supports exactly 3 or 4 regimes")

    df = _load_ipc_lob_frame(ipc_dir)
    feat = _feature_frame(df)
    if len(feat) < cfg.n_regimes * 20:
        raise ValueError("IPC dataset too small for stable regime training")

    x_raw = feat.to_numpy().astype(np.float64)
    scales = x_raw.std(axis=0)
    scales = np.where(scales < 1e-8, 1.0, scales)
    x = x_raw / scales

    labels, centers = _kmeans(x, cfg)
    regime_names = _semantic_regime_names(centers)

    counts = np.bincount(labels, minlength=cfg.n_regimes)
    distribution = {
        regime_names[i]: float(counts[i] / max(len(labels), 1))
        for i in range(cfg.n_regimes)
    }

    artifact = RegimeArtifact(
        version="r2-regime-v1",
        n_regimes=cfg.n_regimes,
        feature_columns=feat.columns,
        regime_names=regime_names,
        centers=centers.tolist(),
        scales=scales.tolist(),
    )
    return artifact, distribution


def save_regime_artifact(artifact: RegimeArtifact, output_path: str) -> None:
    out = Path(output_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(asdict(artifact), indent=2), encoding="utf-8")


def load_regime_artifact(path: str) -> RegimeArtifact:
    payload = json.loads(Path(path).read_text(encoding="utf-8"))
    return RegimeArtifact(**payload)


def infer_regime_probabilities(df: pl.DataFrame, artifact: RegimeArtifact) -> dict[str, float]:
    feat = _feature_frame(df).tail(1)
    if len(feat) == 0:
        return {"p_calm": 1.0, "p_trending": 0.0, "p_shock": 0.0, "p_illiquid": 0.0}

    x_raw = feat.select(artifact.feature_columns).to_numpy().astype(np.float64)[0]
    scales = np.array(artifact.scales, dtype=np.float64)
    x = x_raw / np.where(scales < 1e-8, 1.0, scales)
    centers = np.array(artifact.centers, dtype=np.float64)

    dist = ((centers - x[None, :]) ** 2).sum(axis=1)
    logits = -dist
    logits = logits - np.max(logits)
    probs = np.exp(logits)
    probs = probs / np.sum(probs)

    prob_by_name = {name: float(probs[i]) for i, name in enumerate(artifact.regime_names)}
    p_calm = prob_by_name.get("calm", 0.0)
    p_shock = prob_by_name.get("shock", 0.0)
    p_illiquid = prob_by_name.get("illiquid", 0.0)
    p_trending = prob_by_name.get("trending", 0.0)
    assigned = p_calm + p_shock + p_illiquid + p_trending

    # If some regimes carry non-semantic names (e.g. "regime_1"), their
    # probability mass is not captured by the four named keys.  Distribute
    # the residual proportionally so the returned probabilities sum to 1.0.
    residual = 1.0 - assigned
    if residual > 1e-9:
        total_named = assigned if assigned > 1e-9 else 1.0
        scale = (assigned + residual) / total_named
        p_calm *= scale
        p_shock *= scale
        p_illiquid *= scale
        p_trending *= scale

    return {
        "p_calm": p_calm,
        "p_trending": p_trending,
        "p_shock": p_shock,
        "p_illiquid": p_illiquid,
    }


_REGIME_SIGNAL_SIZE = 48
_REGIME_SEQ_FMT = "=Q"
_REGIME_DATA_FMT = "=ddddq"


class RegimeSignalPublisher:
    def __init__(self, path: str = "/tmp/regime_signal.bin") -> None:
        self._path = path
        self._f = open(path, "w+b")
        self._f.write(b"\x00" * _REGIME_SIGNAL_SIZE)
        self._f.flush()
        self._mm = mmap.mmap(self._f.fileno(), _REGIME_SIGNAL_SIZE)

    def publish(self, p_calm: float, p_trending: float, p_shock: float, p_illiquid: float) -> None:
        seq: int = struct.unpack_from(_REGIME_SEQ_FMT, self._mm, 0)[0]
        struct.pack_into(_REGIME_SEQ_FMT, self._mm, 0, seq + 1)
        struct.pack_into(
            _REGIME_DATA_FMT,
            self._mm,
            8,
            float(p_calm),
            float(p_trending),
            float(p_shock),
            float(p_illiquid),
            time.time_ns(),
        )
        struct.pack_into(_REGIME_SEQ_FMT, self._mm, 0, seq + 2)
        self._mm.flush()

    def close(self) -> None:
        # Close mmap and file handle but intentionally do NOT delete the file.
        # Deleting the signal file on close would create a window during model
        # reloads where the C++ RegimeSignalReader has no file to mmap and
        # falls back to stale/default probabilities.
        try:
            self._mm.close()
        except Exception:
            pass
        try:
            self._f.close()
        except Exception:
            pass

    def __enter__(self) -> "RegimeSignalPublisher":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def __del__(self) -> None:
        self.close()
