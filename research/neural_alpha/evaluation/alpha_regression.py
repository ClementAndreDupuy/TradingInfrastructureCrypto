from __future__ import annotations
from dataclasses import dataclass
import numpy as np
from scipy import stats


@dataclass
class AlphaMetrics:
    ic_mean: float
    ic_std: float
    icir: float
    hit_rate: float
    ols_alpha: float
    ols_beta: float
    ols_t_stat: float
    ols_p_value: float
    r_squared: float
    turnover: float
    n_samples: int

    def __str__(self) -> str:
        return f"IC={self.ic_mean:.4f}  ICIR={self.icir:.4f}  HitRate={self.hit_rate:.3f}  α={self.ols_alpha:.6f} (t={self.ols_t_stat:.2f} p={self.ols_p_value:.4f})  β={self.ols_beta:.4f}  R²={self.r_squared:.4f}  Turnover={self.turnover:.6f}  N={self.n_samples}"


@dataclass
class ConfidenceBinStats:
    lower: float
    upper: float
    count: int
    mean_confidence: float
    empirical_precision: float


@dataclass
class ConfidenceCalibrationReport:
    ece: float
    long_ece: float
    short_ece: float
    signed_ece: float
    coverage: float
    mean_confidence: float
    mean_signed_precision: float
    bins: list[ConfidenceBinStats]


@dataclass
class ThresholdSweepPoint:
    threshold: float
    coverage: float
    hit_rate: float
    ic: float
    mean_abs_signal: float


@dataclass
class FlatThresholdPoint:
    flat_threshold_bps: float
    down_share: float
    flat_share: float
    up_share: float


def compute_ic(
    signals: np.ndarray, returns: np.ndarray, rolling_window: int = 20
) -> tuple[float, float, float]:
    """
    Compute Information Coefficient (Spearman rank correlation).

    Returns:
        ic_mean, ic_std, icir
    """
    signals = np.asarray(signals, dtype=np.float64).ravel()
    returns = np.asarray(returns, dtype=np.float64).ravel()
    valid = np.isfinite(signals) & np.isfinite(returns)
    s = signals[valid]
    r = returns[valid]
    if len(s) < 10:
        return (0.0, 1e-09, 0.0)
    ics: list[float] = []
    for i in range(rolling_window, len(s)):
        window_s = s[i - rolling_window : i]
        window_r = r[i - rolling_window : i]
        (rho, _) = stats.spearmanr(window_s, window_r)
        if np.isfinite(rho):
            ics.append(float(rho))
    if not ics:
        (rho, _) = stats.spearmanr(s, r)
        return (float(rho) if np.isfinite(rho) else 0.0, 1e-09, 0.0)
    ic_arr = np.array(ics)
    ic_mean = float(ic_arr.mean())
    ic_std = float(ic_arr.std(ddof=1)) if len(ic_arr) > 1 else 1e-09
    icir = ic_mean / (ic_std + 1e-09)
    return (ic_mean, ic_std, icir)


def compute_hit_rate(signals: np.ndarray, returns: np.ndarray) -> float:
    """Fraction of ticks where sign(signal) == sign(return)."""
    signals = np.asarray(signals).ravel()
    returns = np.asarray(returns).ravel()
    valid = np.isfinite(signals) & np.isfinite(returns) & (signals != 0)
    if valid.sum() == 0:
        return 0.5
    return float((np.sign(signals[valid]) == np.sign(returns[valid])).mean())


def ols_regression(
    signals: np.ndarray, returns: np.ndarray
) -> tuple[float, float, float, float, float]:
    """
    OLS regression: return ~ alpha + beta * signal

    Returns:
        alpha, beta, t_stat, p_value, r_squared
    """
    signals = np.asarray(signals, dtype=np.float64).ravel()
    returns = np.asarray(returns, dtype=np.float64).ravel()
    valid = np.isfinite(signals) & np.isfinite(returns)
    s = signals[valid]
    r = returns[valid]
    if len(s) < 5:
        return (0.0, 0.0, 0.0, 1.0, 0.0)
    result = stats.linregress(s, r)
    slope = float(result.slope)
    intercept = float(result.intercept)
    r_value = float(result.rvalue)
    n = len(s)
    x_mean = s.mean()
    ss_xx = ((s - x_mean) ** 2).sum()
    residuals = r - (intercept + slope * s)
    s2 = (residuals**2).sum() / (n - 2 + 1e-09)
    se_int = np.sqrt(s2 * (1.0 / n + x_mean**2 / (ss_xx + 1e-09)))
    t_stat = intercept / (se_int + 1e-09)
    p_value = float(2 * stats.t.sf(abs(t_stat), df=n - 2))
    return (intercept, slope, t_stat, p_value, r_value**2)


def compute_turnover(signals: np.ndarray) -> float:
    """Mean absolute change in signal per tick."""
    s = np.asarray(signals, dtype=np.float64).ravel()
    if len(s) < 2:
        return 0.0
    return float(np.abs(np.diff(s)).mean())


def _signed_confidence_arrays(
    fold_results: list[dict],
    horizon_idx: int = 2,
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    confidences: list[np.ndarray] = []
    correct: list[np.ndarray] = []
    signals: list[np.ndarray] = []
    for fold in fold_results:
        preds = np.asarray(fold["predictions"], dtype=np.float32)
        labels = np.asarray(fold["labels"], dtype=np.float32)
        direction_probs = fold.get("direction_probs")
        if direction_probs is None:
            continue
        direction_probs = np.asarray(direction_probs, dtype=np.float32)
        if preds.ndim != 2 or labels.ndim != 2 or direction_probs.ndim != 2 or direction_probs.shape[1] < 3:
            continue
        signal = preds[:, horizon_idx]
        conf = np.where(signal >= 0.0, direction_probs[:, 2], direction_probs[:, 0])
        realised = labels[:, horizon_idx]
        signed_correct = np.where(signal > 0.0, realised > 0.0, np.where(signal < 0.0, realised < 0.0, False))
        active = signal != 0.0
        confidences.append(conf[active])
        correct.append(signed_correct[active].astype(np.float32))
        signals.append(signal[active])
    if not confidences:
        empty = np.zeros(0, dtype=np.float32)
        return (empty, empty, empty)
    return (
        np.concatenate(confidences).astype(np.float32),
        np.concatenate(correct).astype(np.float32),
        np.concatenate(signals).astype(np.float32),
    )


def expected_calibration_error(
    confidences: np.ndarray,
    outcomes: np.ndarray,
    n_bins: int = 10,
) -> tuple[float, list[ConfidenceBinStats]]:
    conf = np.asarray(confidences, dtype=np.float64).ravel()
    obs = np.asarray(outcomes, dtype=np.float64).ravel()
    valid = np.isfinite(conf) & np.isfinite(obs)
    conf = conf[valid]
    obs = obs[valid]
    if len(conf) == 0:
        return (0.0, [])

    bins: list[ConfidenceBinStats] = []
    ece = 0.0
    edges = np.linspace(0.0, 1.0, n_bins + 1)
    for idx in range(n_bins):
        lower = float(edges[idx])
        upper = float(edges[idx + 1])
        if idx == n_bins - 1:
            mask = (conf >= lower) & (conf <= upper)
        else:
            mask = (conf >= lower) & (conf < upper)
        count = int(mask.sum())
        if count == 0:
            continue
        mean_conf = float(conf[mask].mean())
        precision = float(obs[mask].mean())
        ece += abs(mean_conf - precision) * (count / len(conf))
        bins.append(
            ConfidenceBinStats(
                lower=lower,
                upper=upper,
                count=count,
                mean_confidence=mean_conf,
                empirical_precision=precision,
            )
        )
    return (float(ece), bins)


def analyse_direction_calibration(
    fold_results: list[dict],
    horizon_idx: int = 2,
    n_bins: int = 10,
) -> ConfidenceCalibrationReport:
    signed_conf, signed_correct, signed_signal = _signed_confidence_arrays(fold_results, horizon_idx=horizon_idx)
    if len(signed_conf) == 0:
        return ConfidenceCalibrationReport(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, [])

    long_mask = signed_signal > 0.0
    short_mask = signed_signal < 0.0
    signed_ece, bins = expected_calibration_error(signed_conf, signed_correct, n_bins=n_bins)
    long_ece, _ = expected_calibration_error(signed_conf[long_mask], signed_correct[long_mask], n_bins=n_bins)
    short_ece, _ = expected_calibration_error(signed_conf[short_mask], signed_correct[short_mask], n_bins=n_bins)
    total_samples = 0
    total_predictions = 0
    for fold in fold_results:
        preds = np.asarray(fold["predictions"], dtype=np.float32)
        if preds.ndim != 2:
            continue
        total_samples += len(preds)
        total_predictions += int(np.count_nonzero(preds[:, horizon_idx]))
    coverage = float(total_predictions / total_samples) if total_samples > 0 else 0.0
    return ConfidenceCalibrationReport(
        ece=signed_ece,
        long_ece=long_ece,
        short_ece=short_ece,
        signed_ece=signed_ece,
        coverage=coverage,
        mean_confidence=float(signed_conf.mean()),
        mean_signed_precision=float(signed_correct.mean()),
        bins=bins,
    )


def sweep_direction_thresholds(
    fold_results: list[dict],
    thresholds: list[float] | None = None,
    horizon_idx: int = 2,
) -> list[ThresholdSweepPoint]:
    if thresholds is None:
        thresholds = [0.40, 0.45, 0.50, 0.55, 0.60, 0.65, 0.70]
    signed_conf, _, _ = _signed_confidence_arrays(fold_results, horizon_idx=horizon_idx)
    if len(signed_conf) == 0:
        return [ThresholdSweepPoint(t, 0.0, 0.5, 0.0, 0.0) for t in thresholds]

    all_signals: list[np.ndarray] = []
    all_returns: list[np.ndarray] = []
    all_conf: list[np.ndarray] = []
    for fold in fold_results:
        preds = np.asarray(fold["predictions"], dtype=np.float32)
        labels = np.asarray(fold["labels"], dtype=np.float32)
        direction_probs = fold.get("direction_probs")
        if preds.ndim != 2 or labels.ndim != 2 or direction_probs is None:
            continue
        direction_probs = np.asarray(direction_probs, dtype=np.float32)
        conf = np.where(preds[:, horizon_idx] >= 0.0, direction_probs[:, 2], direction_probs[:, 0])
        all_signals.append(preds[:, horizon_idx])
        all_returns.append(labels[:, horizon_idx])
        all_conf.append(conf)
    signals = np.concatenate(all_signals)
    returns = np.concatenate(all_returns)
    confidences = np.concatenate(all_conf)
    total = max(len(signals), 1)
    points: list[ThresholdSweepPoint] = []
    for threshold in thresholds:
        gated = signals.copy()
        gated[(gated > 0.0) & (confidences < threshold)] = 0.0
        gated[(gated < 0.0) & (confidences < threshold)] = 0.0
        active = gated != 0.0
        coverage = float(active.mean()) if len(gated) else 0.0
        hit_rate = compute_hit_rate(gated, returns)
        ic, _, _ = compute_ic(gated, returns)
        mean_abs_signal = float(np.mean(np.abs(gated[active]))) if active.any() else 0.0
        points.append(
            ThresholdSweepPoint(
                threshold=float(threshold),
                coverage=coverage,
                hit_rate=hit_rate,
                ic=ic,
                mean_abs_signal=mean_abs_signal,
            )
        )
    return points


def analyse_flat_threshold_sensitivity(
    fold_results: list[dict],
    thresholds_bps: list[float] | None = None,
    horizon_idx: int = 2,
) -> list[FlatThresholdPoint]:
    if thresholds_bps is None:
        thresholds_bps = [0.1, 0.2, 0.3, 0.5, 1.0]
    mid_returns: list[np.ndarray] = []
    for fold in fold_results:
        labels = np.asarray(fold["labels"], dtype=np.float32)
        if labels.ndim != 2 or labels.shape[1] <= horizon_idx:
            continue
        mid_returns.append(labels[:, horizon_idx])
    if not mid_returns:
        return [FlatThresholdPoint(t, 0.0, 0.0, 0.0) for t in thresholds_bps]
    returns = np.concatenate(mid_returns)
    points: list[FlatThresholdPoint] = []
    for threshold_bps in thresholds_bps:
        threshold = threshold_bps * 0.0001
        down = float(np.mean(returns < -threshold))
        flat = float(np.mean(np.abs(returns) <= threshold))
        up = float(np.mean(returns > threshold))
        points.append(
            FlatThresholdPoint(
                flat_threshold_bps=float(threshold_bps),
                down_share=down,
                flat_share=flat,
                up_share=up,
            )
        )
    return points


def analyse_alpha(
    fold_results: list[dict], horizon_idx: int = 2, rolling_window: int = 20
) -> AlphaMetrics:
    """
    Aggregate alpha analysis across all walk-forward folds.

    Args:
        fold_results  : list of fold dicts from walk_forward_train
        horizon_idx   : which return horizon to use (0=1t, 1=10t, 2=100t, 3=500t)
        rolling_window: window for rolling IC

    Returns:
        AlphaMetrics instance
    """
    all_signals: list[np.ndarray] = []
    all_returns: list[np.ndarray] = []
    for fold in fold_results:
        preds = fold["predictions"]
        labels = fold["labels"]
        all_signals.append(preds[:, horizon_idx])
        all_returns.append(labels[:, horizon_idx])
    if not all_signals:
        return AlphaMetrics(0, 0, 0, 0.5, 0, 0, 0, 1.0, 0, 0, 0)
    signals = np.concatenate(all_signals)
    returns = np.concatenate(all_returns)
    (ic_mean, ic_std, icir) = compute_ic(signals, returns, rolling_window)
    hit_rate = compute_hit_rate(signals, returns)
    (alpha, beta, t, p, r2) = ols_regression(signals, returns)
    turnover = compute_turnover(signals)
    return AlphaMetrics(
        ic_mean=ic_mean,
        ic_std=ic_std,
        icir=icir,
        hit_rate=hit_rate,
        ols_alpha=alpha,
        ols_beta=beta,
        ols_t_stat=t,
        ols_p_value=p,
        r_squared=r2,
        turnover=turnover,
        n_samples=len(signals),
    )


def print_alpha_report(metrics: AlphaMetrics, backtest_metrics: dict | None = None) -> None:
    print("\n" + "=" * 65)
    print("  ALPHA ANALYSIS REPORT")
    print("=" * 65)
    print(
        f"  IC (mean / std / ICIR) : {metrics.ic_mean:+.4f} / {metrics.ic_std:.4f} / {metrics.icir:+.4f}"
    )
    print(f"  Hit rate               : {metrics.hit_rate:.3f}")
    print(f"  OLS alpha (annualised) : {metrics.ols_alpha:.6f}")
    print(f"    t-stat / p-value     : {metrics.ols_t_stat:.2f} / {metrics.ols_p_value:.4f}")
    print(f"  OLS beta               : {metrics.ols_beta:.4f}")
    print(f"  R²                     : {metrics.r_squared:.4f}")
    print(f"  Turnover               : {metrics.turnover:.6f}")
    print(f"  Samples (out-of-sample): {metrics.n_samples:,}")
    if backtest_metrics and "error" not in backtest_metrics:
        print("\n  BACKTEST SUMMARY")
        print(f"  Total trades           : {backtest_metrics.get('total_trades', 0)}")
        print(f"  Total net PnL (USD)    : {backtest_metrics.get('total_net_pnl', 0):.4f}")
        print(f"  Sharpe (annualised)    : {backtest_metrics.get('sharpe_annualised', 0):.3f}")
        print(f"  Win rate               : {backtest_metrics.get('win_rate', 0):.3f}")
        print(f"  Max drawdown (USD)     : {backtest_metrics.get('max_drawdown_usd', 0):.4f}")
        print(f"  Avg slippage (USD)     : {backtest_metrics.get('avg_slippage_usd', 0):.6f}")
    print("=" * 65 + "\n")


def print_direction_diagnostics(
    calibration: ConfidenceCalibrationReport,
    threshold_sweep: list[ThresholdSweepPoint],
    flat_thresholds: list[FlatThresholdPoint],
) -> None:
    print("  DIRECTION CONFIDENCE DIAGNOSTICS")
    print("  " + "-" * 61)
    print(f"  Signed ECE             : {calibration.signed_ece:.4f}")
    print(f"    Long / Short ECE     : {calibration.long_ece:.4f} / {calibration.short_ece:.4f}")
    print(f"  Mean confidence        : {calibration.mean_confidence:.4f}")
    print(f"  Empirical precision    : {calibration.mean_signed_precision:.4f}")
    print(f"  Non-zero signal cover  : {calibration.coverage:.3f}")
    if calibration.bins:
        print("  Reliability bins       :")
        for bin_stat in calibration.bins:
            print(
                f"    [{bin_stat.lower:.2f}, {bin_stat.upper:.2f}]  n={bin_stat.count:4d}  conf={bin_stat.mean_confidence:.3f}  precision={bin_stat.empirical_precision:.3f}"
            )
    if threshold_sweep:
        print("  Threshold sweep        :")
        for point in threshold_sweep:
            print(
                f"    thr={point.threshold:.2f}  coverage={point.coverage:.3f}  hit={point.hit_rate:.3f}  IC={point.ic:+.4f}  |signal|={point.mean_abs_signal:.6f}"
            )
    if flat_thresholds:
        print("  Flat-band sensitivity  :")
        for point in flat_thresholds:
            print(
                f"    ±{point.flat_threshold_bps:.1f}bps -> down={point.down_share:.3f} flat={point.flat_share:.3f} up={point.up_share:.3f}"
            )
    print("=" * 65 + "\n")
