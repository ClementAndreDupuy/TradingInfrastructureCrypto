"""
Neural alpha shadow session.

Runs the trained CryptoAlphaNet model in real-time alongside the C++ shadow
engine. Every poll interval it:
    1. Fetches live L5 LOB snapshots from Binance and Kraken.
    2. Runs inference through the loaded (or freshly trained) model.
    3. Publishes the signal to shared memory for the C++ strategy to gate trades.
    4. Appends the signal + outcome to a JSONL log.
    5. Optionally prints a rolling summary every report_interval seconds.

Shared memory bridge:
    Writes to /tmp/neural_alpha_signal.bin (24 bytes):
        offset  0: float64  signal_bps  — mid-horizon return prediction (bps)
        offset  8: float64  risk_score  — adverse-selection probability [0, 1]
        offset 16: int64    ts_ns       — nanosecond timestamp
    The C++ AlphaSignalReader (core/ipc/alpha_signal.hpp) mmaps this file.

Usage:
    python -m research.alpha.neural_alpha.shadow_session \
        --model-path models/neural_alpha_best.pt \
        --duration 3600 \
        --interval-ms 500

    # Train on the fly (no saved model):
    python -m research.alpha.neural_alpha.shadow_session --train-ticks 500
"""
from __future__ import annotations

import argparse
import json
import mmap
import os
import signal
import struct
import time
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import polars as pl
import torch

from .features import compute_lob_tensor, compute_scalar_features, normalise_scalar
from .model import CryptoAlphaNet
from .pipeline import _fetch_binance_l5, _fetch_kraken_l5, _fetch_solana_l5, generate_synthetic_lob
from .trainer import TrainerConfig, walk_forward_train

# Shared memory file layout: [float64 signal_bps][float64 risk_score][int64 ts_ns]
_SIGNAL_FILE = "/tmp/neural_alpha_signal.bin"
_SIGNAL_FMT  = "ddq"   # double, double, long long
_SIGNAL_SIZE = struct.calcsize(_SIGNAL_FMT)  # 24 bytes


class _SignalPublisher:
    """Writes neural alpha signal to a memory-mapped file every inference tick."""

    def __init__(self, path: str = _SIGNAL_FILE) -> None:
        self._path = path
        self._f = open(path, "w+b")
        self._f.write(b"\x00" * _SIGNAL_SIZE)
        self._f.flush()
        self._mm = mmap.mmap(self._f.fileno(), _SIGNAL_SIZE)

    def publish(self, signal_bps: float, risk_score: float) -> None:
        ts_ns = time.time_ns()
        packed = struct.pack(_SIGNAL_FMT, signal_bps, risk_score, ts_ns)
        self._mm.seek(0)
        self._mm.write(packed)
        self._mm.flush()

    def close(self) -> None:
        try:
            self._mm.close()
            self._f.close()
            os.unlink(self._path)
        except OSError:
            pass


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
    train_ticks: int = 0
    train_epochs: int = 10
    synthetic: bool = False
    exchanges: list[str] = field(default_factory=lambda: ["SOLANA"])
    signal_file: str = _SIGNAL_FILE


class NeuralAlphaShadowSession:
    """
    Runs live neural alpha inference and publishes signals to shared memory
    so the C++ strategy can gate trades in real-time.
    """

    def __init__(self, cfg: ShadowSessionConfig) -> None:
        self.cfg = cfg
        self._device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
        self._model: CryptoAlphaNet | None = None
        self._scalar_mean: np.ndarray | None = None
        self._scalar_std:  np.ndarray | None = None
        self._ring: list[dict] = []
        self._log_fp = open(cfg.log_path, "a")
        self._publisher = _SignalPublisher(cfg.signal_file)
        self._running = False
        self._signals: list[float] = []
        self._outcomes: list[float] = []

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
        norm_path = Path(path).with_name("scalar_norm_stats.npz")
        if norm_path.exists():
            d = np.load(norm_path)
            self._scalar_mean = d["mean"]
            self._scalar_std  = d["std"]
            print(f"Normalisation stats loaded from {norm_path}")
        print(f"Model loaded from {path}")

    def train_on_recent(self, n_ticks: int) -> None:
        print(f"Collecting {n_ticks} ticks for in-place training...")
        if self.cfg.synthetic:
            df = generate_synthetic_lob(n_ticks)
        else:
            rows: list[dict] = []
            interval_s = self.cfg.interval_ms / 1000.0
            _fetchers = {
                "BINANCE": _fetch_binance_l5,
                "KRAKEN": _fetch_kraken_l5,
                "SOLANA": _fetch_solana_l5,
            }
            for i in range(n_ticks):
                for ex in self.cfg.exchanges:
                    fetcher = _fetchers.get(ex)
                    if fetcher:
                        row = fetcher()
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

        scalars = compute_scalar_features(df)
        _, self._scalar_mean, self._scalar_std = normalise_scalar(scalars)
        print(f"In-place training done. Val loss: {best['metrics'].get('loss_total', 'n/a'):.4f}")

    # ── Inference + publish ───────────────────────────────────────────────────

    def _infer(self) -> dict | None:
        if self._model is None or len(self._ring) < self.cfg.seq_len:
            return None

        window = self._ring[-self.cfg.seq_len :]
        df = pl.DataFrame(window)
        lob_np = compute_lob_tensor(df)
        scalar_np = compute_scalar_features(df)

        if self._scalar_mean is not None:
            scalar_np = (scalar_np - self._scalar_mean) / (self._scalar_std + 1e-8)

        lob_t    = torch.from_numpy(lob_np).unsqueeze(0).to(self._device)
        scalar_t = torch.from_numpy(scalar_np).unsqueeze(0).to(self._device)

        with torch.no_grad():
            out = self._model(lob_t, scalar_t)

        ret  = out["returns"][0, -1].cpu().numpy()   # (3,)
        risk = float(out["risk"][0, -1].cpu().item())

        raw_signal_bps = float(ret[1]) * 1e4   # mid-horizon in bps

        # Direction-head gating: require direction confidence > 0.55 to agree
        dir_probs = torch.softmax(out["direction"][0, -1], dim=-1).cpu().numpy()
        # dir_probs: [p_down, p_flat, p_up]
        if raw_signal_bps > 0 and dir_probs[2] > 0.55:
            signal_bps = raw_signal_bps
        elif raw_signal_bps < 0 and dir_probs[0] > 0.55:
            signal_bps = raw_signal_bps
        else:
            signal_bps = 0.0

        # Publish to shared memory — C++ reads on next book update (< 100 ns)
        self._publisher.publish(signal_bps, risk)

        last = window[-1]
        mid = (last.get("best_bid", last.get("bid_price_1", 0.0)) +
               last.get("best_ask", last.get("ask_price_1", 0.0))) / 2.0

        return {
            "timestamp_ns":  last["timestamp_ns"],
            "exchange":      last.get("exchange", "BINANCE"),
            "mid_price":     mid,
            "ret_short_bps": float(ret[0]) * 1e4,
            "ret_mid_bps":   signal_bps,
            "ret_long_bps":  float(ret[2]) * 1e4,
            "risk_score":    risk,
            "signal":        float(ret[1]),
            "dir_p_down":    float(dir_probs[0]),
            "dir_p_flat":    float(dir_probs[1]),
            "dir_p_up":      float(dir_probs[2]),
            "gated":         signal_bps == 0.0 and raw_signal_bps != 0.0,
        }

    # ── Data collection ───────────────────────────────────────────────────────

    def _fetch_tick(self) -> list[dict]:
        ticks: list[dict] = []
        if self.cfg.synthetic:
            row = generate_synthetic_lob(1).row(0, named=True)
            row["timestamp_ns"] = time.time_ns()
            ticks.append(row)
            return ticks
        _fetchers = {
            "BINANCE": _fetch_binance_l5,
            "KRAKEN": _fetch_kraken_l5,
            "SOLANA": _fetch_solana_l5,
        }
        for ex in self.cfg.exchanges:
            fetcher = _fetchers.get(ex)
            if fetcher:
                row = fetcher()
                if row:
                    ticks.append(row)
        return ticks

    # ── Logging ───────────────────────────────────────────────────────────────

    def _log(self, signal_info: dict) -> None:
        self._log_fp.write(json.dumps(signal_info) + "\n")
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
        interval_s  = self.cfg.interval_ms / 1000.0
        end_time    = time.time() + self.cfg.duration_s
        last_report = time.time()
        prev_mid    = None

        print(
            f"Shadow session started — duration={self.cfg.duration_s}s  "
            f"interval={self.cfg.interval_ms}ms  "
            f"signal_file={self.cfg.signal_file}"
        )

        try:
            while self._running and time.time() < end_time:
                tick_start = time.time()

                ticks = self._fetch_tick()
                for tick in ticks:
                    self._ring.append(tick)

                max_ring = self.cfg.seq_len * 2
                if len(self._ring) > max_ring:
                    self._ring = self._ring[-max_ring:]

                signal_info = self._infer()
                if signal_info:
                    if prev_mid is not None and prev_mid > 0:
                        realised = (signal_info["mid_price"] - prev_mid) / prev_mid
                        self._outcomes.append(realised)

                    self._signals.append(signal_info["signal"])
                    prev_mid = signal_info["mid_price"]
                    self._log(signal_info)

                if time.time() - last_report >= self.cfg.report_interval_s:
                    self._print_summary()
                    last_report = time.time()

                time.sleep(max(0.0, interval_s - (time.time() - tick_start)))
        finally:
            self._print_summary()
            self._log_fp.close()
            self._publisher.close()
            print("Shadow session complete.")

    def stop(self) -> None:
        self._running = False


# ── CLI ───────────────────────────────────────────────────────────────────────

def main() -> None:
    ap = argparse.ArgumentParser(description="Neural alpha shadow session")
    ap.add_argument("--model-path",      type=str,   default=None,  dest="model_path")
    ap.add_argument("--log-path",        type=str,   default="neural_alpha_shadow.jsonl",
                    dest="log_path")
    ap.add_argument("--signal-file",     type=str,   default=_SIGNAL_FILE, dest="signal_file",
                    help="Shared memory file path read by C++ AlphaSignalReader")
    ap.add_argument("--interval-ms",     type=int,   default=500,   dest="interval_ms")
    ap.add_argument("--duration",        type=int,   default=3600)
    ap.add_argument("--report-interval", type=int,   default=60,    dest="report_interval_s")
    ap.add_argument("--seq-len",         type=int,   default=64,    dest="seq_len")
    ap.add_argument("--entry-bps",       type=float, default=5.0,   dest="entry_threshold_bps")
    ap.add_argument("--d-spatial",       type=int,   default=64,    dest="d_spatial")
    ap.add_argument("--d-temporal",      type=int,   default=128,   dest="d_temporal")
    ap.add_argument("--train-ticks",     type=int,   default=0,     dest="train_ticks")
    ap.add_argument("--train-epochs",    type=int,   default=10,    dest="train_epochs")
    ap.add_argument("--synthetic",       action="store_true")
    ap.add_argument("--exchanges", type=str, default="SOLANA")
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
        signal_file=args.signal_file,
    )

    session = NeuralAlphaShadowSession(cfg)

    def _handle_sigint(sig, frame):
        print("\nInterrupt received — stopping session...")
        session.stop()

    signal.signal(signal.SIGINT, _handle_sigint)

    if cfg.model_path and Path(cfg.model_path).exists():
        session.load_model(cfg.model_path)
    elif cfg.train_ticks > 0:
        session.train_on_recent(cfg.train_ticks)
    else:
        print(
            "WARNING: no model loaded and --train-ticks not set. "
            "Inference skipped until a model is available."
        )

    session.run()


if __name__ == "__main__":
    main()
