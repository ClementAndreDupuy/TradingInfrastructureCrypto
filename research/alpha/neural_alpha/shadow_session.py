"""
Neural alpha shadow session.

Runs the trained CryptoAlphaNet model in real-time alongside the C++ shadow
engine. Every poll interval it:
    1. Fetches live L5 LOB snapshots from Binance and Kraken.
    2. Runs inference through the loaded (or freshly trained) model.
    3. Appends the signal + outcome to a JSONL log.
    4. Optionally prints a rolling summary every report_interval seconds.

The C++ shadow engine handles order execution and fill simulation. This process
only produces signals — it never submits orders.

Usage:
    python -m research.alpha.neural_alpha.shadow_session \\
        --model-path models/neural_alpha_best.pt \\
        --duration 3600 \\
        --interval-ms 500

    # Train on the fly (no saved model):
    python -m research.alpha.neural_alpha.shadow_session --train-ticks 500
"""
from __future__ import annotations

import argparse
import json
import signal
import time
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import polars as pl
import torch

from .features import compute_lob_tensor, compute_scalar_features, normalise_scalar
from .model import CryptoAlphaNet
from .pipeline import _fetch_binance_l5, _fetch_kraken_l5, generate_synthetic_lob
from .trainer import TrainerConfig, walk_forward_train


@dataclass
class ShadowSessionConfig:
    model_path: str | None = None
    log_path: str = "neural_alpha_shadow.jsonl"
    interval_ms: int = 500
    duration_s: int = 3600
    report_interval_s: int = 60
    seq_len: int = 64
    entry_threshold_bps: float = 5.0
    d_spatial: int = 64
    d_temporal: int = 128
    train_ticks: int = 0          # >0 → train in-place before starting session
    train_epochs: int = 10
    synthetic: bool = False       # use synthetic data (offline testing)
    exchanges: list[str] = field(default_factory=lambda: ["BINANCE", "KRAKEN"])


class NeuralAlphaShadowSession:
    """
    Runs live neural alpha inference and logs signals to JSONL.

    The ring buffer holds the last seq_len LOB snapshots so inference can
    run on every new tick without recomputing the whole history.
    """

    def __init__(self, cfg: ShadowSessionConfig) -> None:
        self.cfg = cfg
        self._device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
        self._model: CryptoAlphaNet | None = None
        self._scalar_mean: np.ndarray | None = None
        self._scalar_std:  np.ndarray | None = None
        self._ring: list[dict] = []          # rolling window of raw tick dicts
        self._log_fp = open(cfg.log_path, "a")
        self._running = False
        self._signals: list[float] = []
        self._outcomes: list[float] = []     # realised mid-return one tick later

    # ── Model loading / training ──────────────────────────────────────────────

    def _build_model(self) -> CryptoAlphaNet:
        return CryptoAlphaNet(
            d_spatial=self.cfg.d_spatial,
            d_temporal=self.cfg.d_temporal,
            seq_len=self.cfg.seq_len,
        ).to(self._device).eval()

    def load_model(self, path: str) -> None:
        model = self._build_model()
        state = torch.load(path, map_location=self._device, weights_only=True)
        model.load_state_dict(state)
        self._model = model
        print(f"Model loaded from {path}")

    def train_on_recent(self, n_ticks: int) -> None:
        print(f"Collecting {n_ticks} ticks for in-place training…")
        if self.cfg.synthetic:
            df = generate_synthetic_lob(n_ticks)
        else:
            rows: list[dict] = []
            interval_s = self.cfg.interval_ms / 1000.0
            for i in range(n_ticks):
                for ex in self.cfg.exchanges:
                    row = _fetch_binance_l5() if ex == "BINANCE" else _fetch_kraken_l5()
                    if row:
                        rows.append(row)
                if i < n_ticks - 1:
                    time.sleep(interval_s)
            if not rows:
                raise RuntimeError("No data collected for training.")
            df = pl.DataFrame(rows).sort("timestamp_ns")

        tcfg = TrainerConfig(
            epochs=self.cfg.train_epochs,
            n_folds=2,
            seq_len=self.cfg.seq_len,
            d_spatial=self.cfg.d_spatial,
            d_temporal=self.cfg.d_temporal,
        )
        fold_results = walk_forward_train(df, tcfg)
        if not fold_results:
            raise RuntimeError("Training produced no fold results — dataset too small.")

        best = min(fold_results, key=lambda f: f["metrics"].get("loss_total", 1e9))
        model = self._build_model()
        model.load_state_dict(best["model_state"])
        self._model = model

        # Compute normalisation stats from full training set
        lob = compute_lob_tensor(df)
        scalars = compute_scalar_features(df)
        _, self._scalar_mean, self._scalar_std = normalise_scalar(scalars)
        print(f"In-place training done. Val loss: {best['metrics'].get('loss_total', 'n/a'):.4f}")

    # ── Inference ─────────────────────────────────────────────────────────────

    def _infer(self) -> dict | None:
        if self._model is None or len(self._ring) < self.cfg.seq_len:
            return None

        window = self._ring[-self.cfg.seq_len :]
        df = pl.DataFrame(window)
        lob_np = compute_lob_tensor(df)       # (T, N_LEVELS, 4)
        scalar_np = compute_scalar_features(df)  # (T, D_SCALAR)

        if self._scalar_mean is not None:
            scalar_np = (scalar_np - self._scalar_mean) / (self._scalar_std + 1e-8)

        lob_t    = torch.from_numpy(lob_np).unsqueeze(0).to(self._device)    # (1, T, L, 4)
        scalar_t = torch.from_numpy(scalar_np).unsqueeze(0).to(self._device) # (1, T, D)

        with torch.no_grad():
            out = self._model(lob_t, scalar_t)

        ret = out["returns"][0, -1].cpu().numpy()      # (3,) — last tick predictions
        risk = float(out["risk"][0, -1].cpu().item())

        last = window[-1]
        mid = (last.get("best_bid", last.get("bid_price_1", 0.0)) +
               last.get("best_ask", last.get("ask_price_1", 0.0))) / 2.0

        return {
            "timestamp_ns":   last["timestamp_ns"],
            "exchange":       last.get("exchange", "BINANCE"),
            "mid_price":      mid,
            "ret_short_bps":  float(ret[0]) * 1e4,
            "ret_mid_bps":    float(ret[1]) * 1e4,
            "ret_long_bps":   float(ret[2]) * 1e4,
            "risk_score":     risk,
            "signal":         float(ret[1]),    # mid-horizon as primary signal
        }

    # ── Data collection ───────────────────────────────────────────────────────

    def _fetch_tick(self) -> list[dict]:
        ticks: list[dict] = []
        if self.cfg.synthetic:
            from .pipeline import generate_synthetic_lob
            row = generate_synthetic_lob(1).row(0, named=True)
            row["timestamp_ns"] = time.time_ns()
            ticks.append(row)
            return ticks
        for ex in self.cfg.exchanges:
            row = _fetch_binance_l5() if ex == "BINANCE" else _fetch_kraken_l5()
            if row:
                ticks.append(row)
        return ticks

    # ── Logging ───────────────────────────────────────────────────────────────

    def _log(self, signal_info: dict) -> None:
        line = json.dumps(signal_info)
        self._log_fp.write(line + "\n")
        self._log_fp.flush()

    # ── Summary ───────────────────────────────────────────────────────────────

    def _print_summary(self) -> None:
        n = len(self._signals)
        if n == 0:
            print("  [shadow] No signals yet.")
            return

        sigs = np.array(self._signals)
        mean_sig = float(np.mean(sigs)) * 1e4
        std_sig  = float(np.std(sigs))  * 1e4

        ic = 0.0
        if len(self._outcomes) >= 10:
            outs = np.array(self._outcomes[:len(self._signals)])
            if outs.std() > 0 and sigs.std() > 0:
                ic = float(np.corrcoef(sigs[:len(outs)], outs)[0, 1])

        print(
            f"  [shadow] ticks={n}  signal_mean={mean_sig:.2f}bps  "
            f"signal_std={std_sig:.2f}bps  IC={ic:.3f}"
        )

    # ── Main loop ─────────────────────────────────────────────────────────────

    def run(self) -> None:
        self._running = True
        interval_s   = self.cfg.interval_ms / 1000.0
        end_time     = time.time() + self.cfg.duration_s
        last_report  = time.time()
        prev_mid     = None

        print(
            f"Shadow session started — duration={self.cfg.duration_s}s  "
            f"interval={self.cfg.interval_ms}ms  log={self.cfg.log_path}"
        )

        while self._running and time.time() < end_time:
            tick_start = time.time()

            ticks = self._fetch_tick()
            for tick in ticks:
                self._ring.append(tick)

            # Keep ring at most 2 * seq_len (enough for one inference pass)
            max_ring = self.cfg.seq_len * 2
            if len(self._ring) > max_ring:
                self._ring = self._ring[-max_ring:]

            signal_info = self._infer()
            if signal_info:
                # Record outcome from previous signal (realised return)
                if prev_mid is not None and prev_mid > 0:
                    realised = (signal_info["mid_price"] - prev_mid) / prev_mid
                    self._outcomes.append(realised)

                self._signals.append(signal_info["signal"])
                prev_mid = signal_info["mid_price"]
                self._log(signal_info)

            # Periodic report
            if time.time() - last_report >= self.cfg.report_interval_s:
                self._print_summary()
                last_report = time.time()

            elapsed = time.time() - tick_start
            sleep_s = max(0.0, interval_s - elapsed)
            time.sleep(sleep_s)

        self._print_summary()
        self._log_fp.close()
        print("Shadow session complete.")

    def stop(self) -> None:
        self._running = False


# ── CLI ───────────────────────────────────────────────────────────────────────

def main() -> None:
    ap = argparse.ArgumentParser(description="Neural alpha shadow session")
    ap.add_argument("--model-path",       type=str,   default=None,  dest="model_path",
                    help="Path to saved model state dict (.pt)")
    ap.add_argument("--log-path",         type=str,   default="neural_alpha_shadow.jsonl",
                    dest="log_path")
    ap.add_argument("--interval-ms",      type=int,   default=500,   dest="interval_ms")
    ap.add_argument("--duration",         type=int,   default=3600,
                    help="Session duration in seconds (default 3600)")
    ap.add_argument("--report-interval",  type=int,   default=60,    dest="report_interval_s")
    ap.add_argument("--seq-len",          type=int,   default=64,    dest="seq_len")
    ap.add_argument("--entry-bps",        type=float, default=5.0,   dest="entry_threshold_bps")
    ap.add_argument("--d-spatial",        type=int,   default=64,    dest="d_spatial")
    ap.add_argument("--d-temporal",       type=int,   default=128,   dest="d_temporal")
    ap.add_argument("--train-ticks",      type=int,   default=0,     dest="train_ticks",
                    help="Collect N ticks and train before session (0 = skip)")
    ap.add_argument("--train-epochs",     type=int,   default=10,    dest="train_epochs")
    ap.add_argument("--synthetic",        action="store_true",
                    help="Use synthetic LOB data (offline testing)")
    ap.add_argument("--exchanges",        type=str,   default="BINANCE,KRAKEN")
    args = ap.parse_args()

    cfg = ShadowSessionConfig(
        model_path=args.model_path,
        log_path=args.log_path,
        interval_ms=args.interval_ms,
        duration_s=args.duration,
        report_interval_s=args.report_interval_s,
        seq_len=args.seq_len,
        entry_threshold_bps=args.entry_threshold_bps,
        d_spatial=args.d_spatial,
        d_temporal=args.d_temporal,
        train_ticks=args.train_ticks,
        train_epochs=args.train_epochs,
        synthetic=args.synthetic,
        exchanges=args.exchanges.split(","),
    )

    session = NeuralAlphaShadowSession(cfg)

    def _handle_sigint(sig, frame):
        print("\nInterrupt received — stopping session…")
        session.stop()

    signal.signal(signal.SIGINT, _handle_sigint)

    if cfg.model_path and Path(cfg.model_path).exists():
        session.load_model(cfg.model_path)
    elif cfg.train_ticks > 0:
        session.train_on_recent(cfg.train_ticks)
    else:
        print(
            "WARNING: no model loaded and --train-ticks not set. "
            "Inference will be skipped until seq_len ticks are buffered "
            "and a model is available."
        )

    session.run()


if __name__ == "__main__":
    main()
