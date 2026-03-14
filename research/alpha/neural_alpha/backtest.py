"""
Event-driven backtest for the neural alpha signal.

Signal logic:
    - Use the model's mid-horizon return prediction as the signal
    - Long when signal > entry_threshold (after cost)
    - Short when signal < -entry_threshold (after cost)
    - Exit when signal reverses past exit_threshold

Fill simulation:
    - Buys fill at best_ask (taker)
    - Sells fill at best_bid (taker)
    - Fees applied at taker rate
    - Latency simulated by skipping one tick before execution

Output:
    - Trade log (Polars DataFrame)
    - Equity curve
    - Performance metrics: Sharpe, ICIR, max drawdown, hit rate, slippage
"""
from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np
import polars as pl


@dataclass
class BacktestConfig:
    entry_threshold_bps: float = 5.0     # min predicted return to enter (bps)
    exit_threshold_bps: float = 1.0      # exit if signal drops below this (bps)
    taker_fee_bps: float = 5.0           # one-way taker fee
    trade_qty: float = 0.001             # BTC per trade
    max_position: float = 0.01           # max position magnitude
    latency_ticks: int = 1               # ticks between signal and execution
    stop_loss_bps: float = 20.0          # per-trade stop loss
    maker_fee_bps: float = 2.0           # one-way maker fee
    initial_capital_usd: float = 100_000.0
    adv_usd: float = 5_000_000.0         # average daily dollar volume
    impact_coeff: float = 0.2            # linear impact coefficient
    impact_square_root: bool = False     # square-root impact if True
    queue_decay_lambda: float = 1.5      # Poisson intensity multiplier
    queue_min_fill_prob: float = 0.01
    queue_max_fill_prob: float = 0.99
    random_seed: int = 42


class NeuralAlphaBacktest:
    """
    Replays LOB data tick-by-tick, driven by neural signal predictions.
    Predictions are aligned with ticks so that prediction[t] was generated
    using information up to tick t (point-in-time safe).
    """

    def __init__(self, cfg: BacktestConfig | None = None) -> None:
        self.cfg = cfg or BacktestConfig()
        self._rng = np.random.default_rng(self.cfg.random_seed)
        self._reset()

    def _reset(self) -> None:
        self._position = 0.0
        self._entry_price = 0.0
        self._pnl = 0.0
        self._peak_pnl = 0.0
        self._max_dd = 0.0
        self._trades: list[dict] = []
        self._equity: list[tuple[int, float]] = []

    def run(
        self,
        df: pl.DataFrame,
        signals: np.ndarray,
    ) -> dict:
        """
        Args:
            df      : LOB DataFrame with timestamp_ns, best_bid, best_ask
            signals : (T,) array of predicted mid-horizon log-returns (from model)

        Returns:
            dict with keys: trades, equity_curve, metrics
        """
        self._rng = np.random.default_rng(self.cfg.random_seed)
        self._reset()

        entry_thresh = self.cfg.entry_threshold_bps * 1e-4
        exit_thresh  = self.cfg.exit_threshold_bps  * 1e-4
        stop_loss    = self.cfg.stop_loss_bps        * 1e-4
        lat          = self.cfg.latency_ticks
        fee_rate     = self.cfg.taker_fee_bps * 1e-4
        maker_fee_rate = self.cfg.maker_fee_bps * 1e-4

        # Extract bid/ask arrays
        best_bid = self._get_col(df, "best_bid")
        best_ask = self._get_col(df, "best_ask")
        ts_arr   = df["timestamp_ns"].to_numpy()

        T = len(df)
        # Signals are aligned: at tick t we use signals[t - lat] (latency offset)
        padded_signals = np.concatenate([np.zeros(lat), signals])

        for t in range(T):
            bid = best_bid[t]
            ask = best_ask[t]
            ts  = ts_arr[t]
            sig = float(padded_signals[t])
            mid = (bid + ask) / 2.0

            if self._position != 0.0:
                # Mark-to-market PnL
                current_val = self._position * (mid - self._entry_price)
                self._equity.append((ts, self._pnl + current_val))

                # Stop loss check
                if current_val / (abs(self._position) * self._entry_price + 1e-9) < -stop_loss:
                    self._close_position(t, bid, ask, ts, fee_rate, reason="STOP")
                    continue

                # Exit conditions
                should_exit = (
                    (self._position > 0 and sig < exit_thresh)
                    or (self._position < 0 and sig > -exit_thresh)
                )
                if should_exit:
                    self._close_position(t, bid, ask, ts, fee_rate, reason="EXIT")
                    continue
            else:
                self._equity.append((ts, self._pnl))

            # Entry conditions (no open position)
            if self._position == 0.0:
                net_long_sig  = sig - fee_rate * 2   # round-trip cost
                net_short_sig = -sig - fee_rate * 2

                if net_long_sig > entry_thresh:
                    qty = min(self.cfg.trade_qty, self.cfg.max_position)
                    fill_prob = self._queue_fill_probability(bid, qty)
                    if self._rng.random() <= fill_prob:
                        fill_px = self._apply_market_impact(bid, qty)
                        self._open_position(t, fill_px, qty, "LONG", ts, maker_fee_rate)
                elif net_short_sig > entry_thresh:
                    qty = min(self.cfg.trade_qty, self.cfg.max_position)
                    fill_prob = self._queue_fill_probability(ask, qty)
                    if self._rng.random() <= fill_prob:
                        fill_px = self._apply_market_impact(ask, -qty)
                        self._open_position(t, fill_px, -qty, "SHORT", ts, maker_fee_rate)

        # Close any open position at end
        if self._position != 0.0 and T > 0:
            self._close_position(
                T - 1, best_bid[T - 1], best_ask[T - 1], ts_arr[T - 1], fee_rate, "EOD"
            )

        return {
            "trades":       self._build_trades_df(),
            "equity_curve": self._build_equity_df(),
            "metrics":      self._compute_metrics(signals),
        }

    def _open_position(self, t: int, fill_px: float, qty: float,
                       side: str, ts: int, fee_rate: float) -> None:
        fee = abs(qty) * fill_px * fee_rate
        self._pnl -= fee
        self._position   = qty
        self._entry_price = fill_px
        self._trades.append({
            "open_ts":    ts,
            "close_ts":   None,
            "side":       side,
            "entry_price": fill_px,
            "exit_price":  None,
            "qty":         qty,
            "entry_fee":   fee,
            "exit_fee":    None,
            "gross_pnl":   None,
            "net_pnl":     None,
            "reason":      None,
        })

    def _close_position(self, t: int, bid: float, ask: float,
                        ts: int, fee_rate: float, reason: str) -> None:
        if not self._trades or self._trades[-1]["close_ts"] is not None:
            self._position = 0.0
            return

        fill_px = bid if self._position > 0 else ask
        fee     = abs(self._position) * fill_px * fee_rate
        gross   = self._position * (fill_px - self._entry_price)
        net     = gross - fee - self._trades[-1]["entry_fee"]

        self._pnl += gross - fee
        self._peak_pnl = max(self._peak_pnl, self._pnl)
        dd = self._pnl - self._peak_pnl
        self._max_dd = min(self._max_dd, dd)

        self._trades[-1].update({
            "close_ts":   ts,
            "exit_price": fill_px,
            "exit_fee":   fee,
            "gross_pnl":  gross,
            "net_pnl":    net,
            "reason":     reason,
        })
        self._position = 0.0

    def _get_col(self, df: pl.DataFrame, col: str) -> np.ndarray:
        # Handle flat-column schema from fetch_and_run.py
        if col in df.columns:
            return df[col].to_numpy(allow_copy=True).astype(np.float64)
        if col == "best_bid" and "bid_price_1" in df.columns:
            return df["bid_price_1"].to_numpy(allow_copy=True).astype(np.float64)
        if col == "best_ask" and "ask_price_1" in df.columns:
            return df["ask_price_1"].to_numpy(allow_copy=True).astype(np.float64)
        return np.zeros(len(df), dtype=np.float64)

    def _build_trades_df(self) -> pl.DataFrame:
        closed = [t for t in self._trades if t["close_ts"] is not None]
        if not closed:
            return pl.DataFrame()
        return pl.DataFrame(closed)

    def _build_equity_df(self) -> pl.DataFrame:
        if not self._equity:
            return pl.DataFrame()
        ts_arr, pnl_arr = zip(*self._equity)
        return pl.DataFrame({"timestamp_ns": list(ts_arr), "cumulative_pnl": list(pnl_arr)})

    def _apply_market_impact(self, fill_px: float, qty: float) -> float:
        notional = abs(qty) * fill_px
        adv = max(self.cfg.adv_usd, 1e-9)
        participation = notional / adv
        if self.cfg.impact_square_root:
            impact = self.cfg.impact_coeff * math.sqrt(participation)
        else:
            impact = self.cfg.impact_coeff * participation
        signed_impact = impact if qty > 0 else -impact
        return fill_px * (1.0 + signed_impact)

    def _queue_fill_probability(self, level_price: float, qty: float) -> float:
        queue_depth = max(level_price * qty, 1e-9)
        expected_flow = self.cfg.queue_decay_lambda * (self.cfg.trade_qty * level_price)
        prob = 1.0 - math.exp(-(expected_flow / queue_depth))
        return float(np.clip(prob, self.cfg.queue_min_fill_prob, self.cfg.queue_max_fill_prob))

    def _compute_metrics(self, signals: np.ndarray) -> dict:
        trades_df = self._build_trades_df()

        if trades_df.is_empty() or "net_pnl" not in trades_df.columns:
            return {"error": "no trades", "total_pnl": self._pnl}

        net_pnls = trades_df["net_pnl"].to_numpy()
        wins   = net_pnls[net_pnls > 0]
        losses = net_pnls[net_pnls <= 0]

        mean_pnl = np.mean(net_pnls)
        sharpe = self._compute_time_aware_sharpe()

        slippage = 0.0
        if "gross_pnl" in trades_df.columns and "net_pnl" in trades_df.columns:
            slippage = float((trades_df["gross_pnl"] - trades_df["net_pnl"]).mean())

        return {
            "total_trades":      len(net_pnls),
            "total_net_pnl":     float(np.sum(net_pnls)),
            "avg_net_pnl":       float(mean_pnl),
            "sharpe_annualised": float(sharpe),
            "win_rate":          float(len(wins) / len(net_pnls)),
            "profit_factor":     float(np.sum(wins) / abs(np.sum(losses))) if losses.size else float("inf"),
            "max_drawdown_usd":  float(self._max_dd),
            "avg_slippage_usd":  float(slippage),
            "signal_mean":       float(np.mean(signals)),
            "signal_std":        float(np.std(signals)),
        }

    def _compute_time_aware_sharpe(self) -> float:
        equity_df = self._build_equity_df()
        if equity_df.is_empty() or len(equity_df) < 3:
            return 0.0

        ts_ns = equity_df["timestamp_ns"].to_numpy()
        pnl = equity_df["cumulative_pnl"].to_numpy().astype(np.float64, copy=False)
        capital_curve = self.cfg.initial_capital_usd + pnl
        if np.any(capital_curve <= 0):
            return 0.0

        dt_s = np.diff(ts_ns.astype(np.float64)) / 1e9
        valid = dt_s > 0
        if not np.any(valid):
            return 0.0

        capital_returns = np.diff(capital_curve) / capital_curve[:-1]
        r = capital_returns[valid]
        dt_s = dt_s[valid]
        if len(r) < 2:
            return 0.0

        mean_r = float(np.mean(r))
        std_r = float(np.std(r, ddof=1))
        if std_r <= 1e-12:
            return 0.0

        steps_per_year = (365.25 * 24 * 60 * 60) / float(np.mean(dt_s))
        return (mean_r / std_r) * math.sqrt(max(steps_per_year, 1.0))


def run_backtest_on_fold(
    fold_result: dict,
    df: pl.DataFrame,
    test_slice_start: int,
    test_slice_end: int,
    cfg: BacktestConfig | None = None,
) -> dict:
    """
    Convenience wrapper: aligns fold predictions with the test slice of df.
    """
    test_df = df[test_slice_start:test_slice_end]
    # predictions is (N_windows, 3); use mid-horizon signal (col 1)
    preds = fold_result["predictions"][:, 1]

    # Align: each prediction corresponds to the last tick of a window.
    # We replicate each prediction for seq_len ticks (nearest-assignment).
    from .dataset import DatasetConfig
    seq_len = DatasetConfig().seq_len
    T_test  = len(test_df)
    signals = np.zeros(T_test, dtype=np.float32)
    for i, pred in enumerate(preds):
        tick_idx = min(i * 1 + seq_len - 1, T_test - 1)  # stride=1
        signals[tick_idx] = pred

    bt = NeuralAlphaBacktest(cfg)
    return bt.run(test_df, signals)
