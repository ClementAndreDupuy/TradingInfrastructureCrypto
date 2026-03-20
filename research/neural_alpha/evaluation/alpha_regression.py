"""
Alpha regression and signal quality analysis.

Metrics computed:
    IC          — Spearman rank correlation between signal and realised return
    ICIR        — IC / rolling_std(IC)   (information ratio of the IC)
    Hit rate    — fraction of ticks where sign(signal) == sign(return)
    OLS alpha   — intercept of signal ~ constant + return regression (statistically tested)
    Beta        — slope of signal ~ return
    t-stat      — t-statistic of the alpha
    Turnover    — mean absolute change in signal per tick

All metrics are computed over out-of-sample fold predictions.
"""

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
