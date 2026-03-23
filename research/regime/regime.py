from __future__ import annotations
import json
import mmap
import struct
import time
from dataclasses import asdict, dataclass
from pathlib import Path
import numpy as np
import polars as pl

_EPS = 1e-12


@dataclass
class RegimeConfig:
    n_regimes: int = 4
    max_iter: int = 75
    tol: float = 0.0001
    random_seed: int = 7
    covariance_floor: float = 1e-06


@dataclass
class RegimeArtifact:
    version: str
    n_regimes: int
    feature_columns: list[str]
    regime_names: list[str]
    initial_probs: list[float]
    transition_matrix: list[list[float]]
    means: list[list[float]]
    variances: list[list[float]]
    scales: list[float]


def _feature_frame(df: pl.DataFrame) -> pl.DataFrame:
    base = df.with_columns(
        [
            ((pl.col("best_bid") + pl.col("best_ask")) / 2.0).alias("mid"),
            (pl.col("best_ask") - pl.col("best_bid")).clip(lower_bound=1e-08).alias("spread"),
            (pl.col("bid_size_1") + pl.col("ask_size_1")).alias("top_depth"),
            (
                (pl.col("bid_size_1") - pl.col("ask_size_1"))
                / (pl.col("bid_size_1") + pl.col("ask_size_1") + 1e-08)
            ).alias("queue_imbalance"),
        ]
    )
    feat = base.with_columns(
        [pl.col("mid").log().diff().fill_null(0.0).alias("log_ret_1")]
    ).with_columns(
        [
            pl.col("log_ret_1").rolling_std(window_size=20).fill_null(0.0).alias("vol_20"),
            pl.col("spread")
            .rolling_mean(window_size=20)
            .fill_null(strategy="backward")
            .fill_null(0.0)
            .alias("spread_20"),
            pl.col("top_depth")
            .rolling_mean(window_size=20)
            .fill_null(strategy="backward")
            .fill_null(0.0)
            .alias("depth_20"),
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
        frame = pl.read_parquet(path) if path.suffix == ".parquet" else pl.read_csv(path)
        if required.issubset(set(frame.columns)):
            frames.append(frame)
    if not frames:
        raise ValueError(f"No IPC files in {ipc_dir} had required LOB columns")
    lob = pl.concat(frames, how="vertical_relaxed").sort("timestamp_ns")
    null_cols = [
        c for c in ("best_bid", "best_ask", "bid_size_1", "ask_size_1") if lob[c].is_null().any()
    ]
    if null_cols:
        raise ValueError(f"Null values found in LOB columns: {null_cols}")
    return lob


def _kmeans_init(x: np.ndarray, cfg: RegimeConfig) -> np.ndarray:
    rng = np.random.default_rng(cfg.random_seed)
    centers = x[rng.choice(len(x), size=cfg.n_regimes, replace=False)].copy()
    for _ in range(max(10, cfg.max_iter // 3)):
        dist = ((x[:, None, :] - centers[None, :, :]) ** 2).sum(axis=2)
        labels = np.argmin(dist, axis=1)
        new_centers = centers.copy()
        for k in range(cfg.n_regimes):
            mask = labels == k
            if np.any(mask):
                new_centers[k] = x[mask].mean(axis=0)
        shift = float(np.linalg.norm(new_centers - centers))
        centers = new_centers
        if shift < cfg.tol:
            break
    return centers


def _logsumexp(arr: np.ndarray, axis: int) -> np.ndarray:
    maxv = np.max(arr, axis=axis, keepdims=True)
    stable = arr - maxv
    return np.squeeze(maxv, axis=axis) + np.log(np.sum(np.exp(stable), axis=axis) + _EPS)


def _log_gaussian_diag(x: np.ndarray, means: np.ndarray, variances: np.ndarray) -> np.ndarray:
    inv_var = 1.0 / np.maximum(variances, _EPS)
    diff = x[:, None, :] - means[None, :, :]
    quadratic = np.sum(diff * diff * inv_var[None, :, :], axis=2)
    log_det = np.sum(np.log(2.0 * np.pi * np.maximum(variances, _EPS)), axis=1)
    return -0.5 * (quadratic + log_det[None, :])


def _forward_backward(
    emissions: np.ndarray, log_initial: np.ndarray, log_transition: np.ndarray
) -> tuple[np.ndarray, np.ndarray, np.ndarray, float]:
    (n_samples, n_states) = emissions.shape
    alpha = np.empty((n_samples, n_states), dtype=np.float64)
    alpha[0] = log_initial + emissions[0]
    for t in range(1, n_samples):
        scores = alpha[t - 1][:, None] + log_transition
        alpha[t] = emissions[t] + _logsumexp(scores, axis=0)
    beta = np.empty((n_samples, n_states), dtype=np.float64)
    beta[-1] = 0.0
    for t in range(n_samples - 2, -1, -1):
        scores = log_transition + emissions[t + 1][None, :] + beta[t + 1][None, :]
        beta[t] = _logsumexp(scores, axis=1)
    ll = float(_logsumexp(alpha[-1], axis=0))
    gamma_log = alpha + beta - ll
    gamma = np.exp(gamma_log)
    xi = np.empty((n_samples - 1, n_states, n_states), dtype=np.float64)
    for t in range(n_samples - 1):
        joint = (
            alpha[t][:, None]
            + log_transition
            + emissions[t + 1][None, :]
            + beta[t + 1][None, :]
            - ll
        )
        xi[t] = np.exp(joint)
    return (gamma, xi, alpha, ll)


def _fit_hmm(
    x: np.ndarray, cfg: RegimeConfig
) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    (n_samples, n_features) = x.shape
    if n_samples < cfg.n_regimes * 20:
        raise ValueError("IPC dataset too small for stable regime training")
    means = _kmeans_init(x, cfg)
    variances = np.full(
        (cfg.n_regimes, n_features), np.var(x, axis=0) + cfg.covariance_floor, dtype=np.float64
    )
    initial = np.full(cfg.n_regimes, 1.0 / cfg.n_regimes, dtype=np.float64)
    transition = np.full((cfg.n_regimes, cfg.n_regimes), 1.0 / cfg.n_regimes, dtype=np.float64)
    prev_ll: float | None = None
    for _ in range(cfg.max_iter):
        emissions = _log_gaussian_diag(x, means, variances)
        (gamma, xi, _, ll) = _forward_backward(
            emissions=emissions,
            log_initial=np.log(np.maximum(initial, _EPS)),
            log_transition=np.log(np.maximum(transition, _EPS)),
        )
        gamma_sum = gamma.sum(axis=0)
        initial = np.maximum(gamma[0], _EPS)
        initial = initial / initial.sum()
        transition = xi.sum(axis=0)
        transition = transition / np.maximum(transition.sum(axis=1, keepdims=True), _EPS)
        transition = np.maximum(transition, _EPS)
        transition = transition / transition.sum(axis=1, keepdims=True)
        means = gamma.T @ x / np.maximum(gamma_sum[:, None], _EPS)
        diff = x[:, None, :] - means[None, :, :]
        weighted_var = (gamma[:, :, None] * (diff * diff)).sum(axis=0)
        variances = weighted_var / np.maximum(gamma_sum[:, None], _EPS)
        variances = np.maximum(variances, cfg.covariance_floor)
        if prev_ll is not None and abs(ll - prev_ll) < cfg.tol:
            break
        prev_ll = ll
    emissions = _log_gaussian_diag(x, means, variances)
    (gamma, _, _, _) = _forward_backward(
        emissions=emissions,
        log_initial=np.log(np.maximum(initial, _EPS)),
        log_transition=np.log(np.maximum(transition, _EPS)),
    )
    labels = np.argmax(gamma, axis=1)
    return (labels, initial, transition, means, variances)


def _calm_anchored_fallback(
    feat: pl.DataFrame, cfg: RegimeConfig, raw_scales: np.ndarray
) -> tuple["RegimeArtifact", dict[str, float]]:
    """
    When training data is flat/degenerate, the market is calm by definition.
    Build a valid HMM artifact anchored to calm rather than raising an error.

    All states share the observed means (only calm data was seen so we cannot
    learn regime separations). Calm gets near-certain initial and transition
    probability, so live inference will output high p_calm for flat data and
    allow probability to flow to other states as real variation appears.
    """
    n = cfg.n_regimes
    x_raw = feat.to_numpy().astype(np.float64)
    obs_mean = x_raw.mean(axis=0)
    obs_var = np.maximum(x_raw.var(axis=0), cfg.covariance_floor)
    scales = np.where(raw_scales < 1e-08, 1.0, raw_scales)
    all_names = ["calm", "trending", "illiquid", "shock"]
    regime_names = all_names[:n]
    calm_idx = 0
    means = np.tile(obs_mean, (n, 1))
    variances = np.tile(obs_var, (n, 1))
    initial = np.full(n, 1e-06)
    initial[calm_idx] = 1.0 - (n - 1) * 1e-06
    initial /= initial.sum()
    transition = np.full((n, n), 0.1 / max(n - 1, 1))
    np.fill_diagonal(transition, 0.9)
    transition /= transition.sum(axis=1, keepdims=True)
    distribution: dict[str, float] = {name: 0.0 for name in regime_names}
    distribution["calm"] = 1.0
    artifact = RegimeArtifact(
        version="r2-regime-hmm-v1",
        n_regimes=n,
        feature_columns=feat.columns,
        regime_names=regime_names,
        initial_probs=initial.tolist(),
        transition_matrix=transition.tolist(),
        means=means.tolist(),
        variances=variances.tolist(),
        scales=scales.tolist(),
    )
    import sys
    spread_means = [round(float(means[i][1]), 6) for i in range(cfg.n_regimes)]
    print(
        f"[Regime] trained  names={regime_names}  spread_means={spread_means}",
        file=sys.stderr,
    )
    return (artifact, distribution)


def _semantic_regime_names(raw_means: np.ndarray) -> list[str]:
    n = raw_means.shape[0]
    names = [f"regime_{i}" for i in range(n)]

    def _assign_first_free(ranked: np.ndarray, label: str) -> None:
        for idx in reversed(ranked):
            if names[idx].startswith("regime_"):
                names[idx] = label
                return

    vol_rank = np.argsort(raw_means[:, 0])
    names[vol_rank[0]] = "calm"
    _assign_first_free(vol_rank, "shock")
    if n >= 3:
        _assign_first_free(np.argsort(raw_means[:, 1]), "illiquid")
    if n >= 4:
        _assign_first_free(np.argsort(raw_means[:, 2]), "trending")
    return names


def train_regime_model_from_df(
    df: pl.DataFrame, cfg: RegimeConfig
) -> tuple[RegimeArtifact, dict[str, float]]:
    if cfg.n_regimes not in {3, 4}:
        raise ValueError("R2 regime model supports exactly 3 or 4 regimes")
    feat = _feature_frame(df)
    x_raw = feat.to_numpy().astype(np.float64)
    raw_scales = x_raw.std(axis=0)
    n_degenerate = int(np.sum(raw_scales < 1e-08))
    if n_degenerate > len(raw_scales) // 2:
        print(
            f"[REGIME] Flat training data ({n_degenerate}/{len(raw_scales)} features near-zero variance) — anchoring artifact to calm regime."
        )
        return _calm_anchored_fallback(feat, cfg, raw_scales)
    scales = np.where(raw_scales < 1e-08, 1.0, raw_scales)
    x = x_raw / scales
    (labels, initial, transition, means, variances) = _fit_hmm(x, cfg)
    regime_names = _semantic_regime_names(means)
    counts = np.bincount(labels, minlength=cfg.n_regimes)
    distribution = {
        regime_names[i]: float(counts[i] / max(len(labels), 1)) for i in range(cfg.n_regimes)
    }
    artifact = RegimeArtifact(
        version="r2-regime-hmm-v1",
        n_regimes=cfg.n_regimes,
        feature_columns=feat.columns,
        regime_names=regime_names,
        initial_probs=initial.tolist(),
        transition_matrix=transition.tolist(),
        means=means.tolist(),
        variances=variances.tolist(),
        scales=scales.tolist(),
    )
    return (artifact, distribution)


def train_regime_model_from_ipc(
    ipc_dir: str, cfg: RegimeConfig
) -> tuple[RegimeArtifact, dict[str, float]]:
    if cfg.n_regimes not in {3, 4}:
        raise ValueError("R2 regime model supports exactly 3 or 4 regimes")
    df = _load_ipc_lob_frame(ipc_dir)
    feat = _feature_frame(df)
    x_raw = feat.to_numpy().astype(np.float64)
    scales = x_raw.std(axis=0)
    scales = np.where(scales < 1e-08, 1.0, scales)
    x = x_raw / scales
    (labels, initial, transition, means, variances) = _fit_hmm(x, cfg)
    regime_names = _semantic_regime_names(means)
    counts = np.bincount(labels, minlength=cfg.n_regimes)
    distribution = {
        regime_names[i]: float(counts[i] / max(len(labels), 1)) for i in range(cfg.n_regimes)
    }
    artifact = RegimeArtifact(
        version="r2-regime-hmm-v1",
        n_regimes=cfg.n_regimes,
        feature_columns=feat.columns,
        regime_names=regime_names,
        initial_probs=initial.tolist(),
        transition_matrix=transition.tolist(),
        means=means.tolist(),
        variances=variances.tolist(),
        scales=scales.tolist(),
    )
    return (artifact, distribution)


def _normalize_prob_vector(values: np.ndarray) -> np.ndarray:
    clipped = np.maximum(values.astype(np.float64), _EPS)
    return clipped / np.maximum(clipped.sum(), _EPS)


def _normalize_transition_matrix(values: np.ndarray) -> np.ndarray:
    clipped = np.maximum(values.astype(np.float64), _EPS)
    denom = np.maximum(clipped.sum(axis=1, keepdims=True), _EPS)
    return clipped / denom


def _validate_artifact(artifact: RegimeArtifact) -> None:
    n = artifact.n_regimes
    if n not in {3, 4}:
        raise ValueError("Regime artifact must contain 3 or 4 regimes")
    if len(artifact.regime_names) != n:
        raise ValueError("Regime artifact regime_names length mismatch")
    if len(set(artifact.regime_names)) != len(artifact.regime_names):
        raise ValueError("Regime artifact regime_names must be unique")
    if len(artifact.initial_probs) != n:
        raise ValueError("Regime artifact initial_probs length mismatch")
    if len(artifact.transition_matrix) != n or any(
        (len(row) != n for row in artifact.transition_matrix)
    ):
        raise ValueError("Regime artifact transition_matrix shape mismatch")
    if len(artifact.means) != n or len(artifact.variances) != n:
        raise ValueError("Regime artifact emission parameter shape mismatch")


def _upgrade_legacy_artifact(payload: dict[str, object]) -> dict[str, object]:
    if "centers" not in payload:
        return payload
    centers = np.array(payload.get("centers", []), dtype=np.float64)
    if centers.ndim != 2 or centers.shape[0] == 0:
        raise ValueError("Legacy regime artifact has invalid centers")
    n_regimes = int(payload.get("n_regimes", centers.shape[0]))
    n_features = int(centers.shape[1])
    upgraded = dict(payload)
    upgraded["version"] = "r2-regime-hmm-v1-legacy-upgraded"
    upgraded["means"] = centers.tolist()
    upgraded["variances"] = np.ones((n_regimes, n_features), dtype=np.float64).tolist()
    upgraded["initial_probs"] = np.full(n_regimes, 1.0 / n_regimes, dtype=np.float64).tolist()
    upgraded["transition_matrix"] = (
        np.eye(n_regimes, dtype=np.float64) * 0.9 + 0.1 / n_regimes
    ).tolist()
    upgraded.pop("centers", None)
    return upgraded


def save_regime_artifact(artifact: RegimeArtifact, output_path: str) -> None:
    _validate_artifact(artifact)
    out = Path(output_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(asdict(artifact), indent=2), encoding="utf-8")


def load_regime_artifact(path: str) -> RegimeArtifact:
    payload = json.loads(Path(path).read_text(encoding="utf-8"))
    payload = _upgrade_legacy_artifact(payload)
    artifact = RegimeArtifact(**payload)
    _validate_artifact(artifact)
    return artifact


def infer_regime_probabilities(df: pl.DataFrame, artifact: RegimeArtifact) -> dict[str, float]:
    feat = _feature_frame(df)
    if len(feat) == 0:
        return {"p_calm": 1.0, "p_trending": 0.0, "p_shock": 0.0, "p_illiquid": 0.0}
    x_raw = feat.select(artifact.feature_columns).to_numpy().astype(np.float64)
    scales = np.array(artifact.scales, dtype=np.float64)
    x = x_raw / np.where(scales < 1e-08, 1.0, scales)
    means = np.array(artifact.means, dtype=np.float64)
    variances = np.array(artifact.variances, dtype=np.float64)
    transition = _normalize_transition_matrix(
        np.array(artifact.transition_matrix, dtype=np.float64)
    )
    initial = _normalize_prob_vector(np.array(artifact.initial_probs, dtype=np.float64))
    emissions = _log_gaussian_diag(x, means, variances)
    (gamma, _, _, _) = _forward_backward(
        emissions=emissions,
        log_initial=np.log(np.maximum(initial, _EPS)),
        log_transition=np.log(np.maximum(transition, _EPS)),
    )
    last_probs = gamma[-1]
    last_probs = last_probs / np.maximum(last_probs.sum(), _EPS)
    prob_by_name = {name: float(last_probs[i]) for (i, name) in enumerate(artifact.regime_names)}
    p_calm = prob_by_name.get("calm", 0.0)
    p_shock = prob_by_name.get("shock", 0.0)
    p_illiquid = prob_by_name.get("illiquid", 0.0)
    p_trending = prob_by_name.get("trending", 0.0)
    assigned = p_calm + p_shock + p_illiquid + p_trending
    residual = 1.0 - assigned
    if residual > 1e-09:
        total_named = assigned if assigned > 1e-09 else 1.0
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

    def __init__(self, path: str = "/tmp/trt_ipc/regime_signal.bin") -> None:
        self._path = path
        Path(path).expanduser().resolve().parent.mkdir(parents=True, exist_ok=True)
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
