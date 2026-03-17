"""
Neural alpha shadow session.

Runs the trained CryptoAlphaNet model in real-time alongside the C++ shadow
engine. Every poll interval it:
    1. Fetches live L5 LOB snapshots from configured exchange venues/symbol.
    2. Runs inference through the loaded (or freshly trained) model.
    3. Publishes the signal to shared memory for the C++ strategy to gate trades.
    4. Appends the signal + outcome to a JSONL log.
    5. Optionally prints a rolling summary every report_interval seconds.

Signal gating:
    - Direction-head confidence > 0.55 (long: dir_up > 0.55, short: dir_down > 0.55)
    - 1-tick and mid-horizon (100t) returns must agree in direction

Ensemble:
    - Optional secondary (smaller) model; signals averaged before gating.

Shared memory bridge:
    Writes to /tmp/neural_alpha_signal.bin (32 bytes, seqlock-protected):
        offset  0: uint64   seq         — seqlock counter (even=stable, odd=writing)
        offset  8: float64  signal_bps  — mid-horizon return prediction (bps)
        offset 16: float64  risk_score  — adverse-selection probability [0, 1]
        offset 24: int64    ts_ns       — nanosecond timestamp
    The C++ AlphaSignalReader (core/ipc/alpha_signal.hpp) mmaps this file.

Usage:
    python -m research.neural_alpha.shadow_session \
        --model-path models/neural_alpha_latest.pt \
        --secondary-model-path models/neural_alpha_secondary.pt \
        --duration 86400 \
        --interval-ms 500 \
        --symbol BTCUSDT \
        --exchanges BINANCE,KRAKEN,OKX,COINBASE
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
from torch.utils.data import DataLoader

from .dataset import DatasetConfig, LOBDataset
from .dataset import rolling_normalise
from .features import compute_lob_tensor, compute_scalar_features
from .governance import ChampionChallengerRegistry, DriftGuard
from .model import CryptoAlphaNet
from .regime import RegimeSignalPublisher, infer_regime_probabilities, load_regime_artifact
from .pipeline import (
    _fetch_binance_l5,
    _fetch_coinbase_l5,
    _fetch_kraken_l5,
    _fetch_okx_l5,
    _fetch_solana_l5,
    collect_from_core_bridge,
)
from .core_bridge import CoreBridge
from .trainer import TrainerConfig, walk_forward_train

# Shared memory file layout (32 bytes, seqlock-protected):
#   offset  0: uint64  seq        — seqlock counter (even=stable, odd=write in progress)
#   offset  8: float64 signal_bps — mid-horizon return prediction (bps)
#   offset 16: float64 risk_score — adverse-selection probability [0, 1]
#   offset 24: int64   ts_ns      — nanosecond timestamp of last update
#
# Write protocol: increment seq to odd → write fields → increment seq to even.
# The C++ AlphaSignalReader spins until seq is even and stable across the read.
_SIGNAL_FILE = "/tmp/neural_alpha_signal.bin"
_SIGNAL_SIZE = 32  # uint64 + float64 + float64 + int64
_SEQ_FMT = "=Q"    # native-endian uint64 (seqlock counter)
_SEQ_OFFSET = 0
_DATA_FMT = "=ddq" # native-endian: float64 signal_bps, float64 risk_score, int64 ts_ns
_DATA_OFFSET = 8

_REGIME_SIGNAL_FILE = "/tmp/regime_signal.bin"


class _SignalPublisher:
    """Writes neural alpha signal to a seqlock-protected memory-mapped file."""

    def __init__(self, path: str = _SIGNAL_FILE) -> None:
        self._path = path
        self._f = open(path, "w+b")
        self._f.write(b"\x00" * _SIGNAL_SIZE)
        self._f.flush()
        self._mm = mmap.mmap(self._f.fileno(), _SIGNAL_SIZE)

    def publish(self, signal_bps: float, risk_score: float) -> None:
        ts_ns = time.time_ns()
        # Seqlock write protocol (single writer assumed):
        #   1. read current seq (always even when no write is in flight)
        #   2. write seq+1 (odd)  → signals write-in-progress to C++ readers
        #   3. write data fields
        #   4. write seq+2 (even) → signals write-complete; readers can now consume
        seq: int = struct.unpack_from(_SEQ_FMT, self._mm, _SEQ_OFFSET)[0]
        struct.pack_into(_SEQ_FMT, self._mm, _SEQ_OFFSET, seq + 1)
        struct.pack_into(_DATA_FMT, self._mm, _DATA_OFFSET, signal_bps, risk_score, ts_ns)
        struct.pack_into(_SEQ_FMT, self._mm, _SEQ_OFFSET, seq + 2)
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
    secondary_model_path: str | None = None
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
    symbol: str = "BTCUSDT"
    exchanges: list[str] = field(default_factory=lambda: ["BINANCE", "KRAKEN", "OKX", "COINBASE"])
    signal_file: str = _SIGNAL_FILE
    regime_signal_file: str = _REGIME_SIGNAL_FILE
    regime_model_path: str = "models/r2_regime_model.json"
    registry_path: str = "models/model_registry.json"
    drift_window: int = 200
    drift_min_samples: int = 60
    drift_ic_floor: float = -0.05
    safe_mode_ticks: int = 120
    continuous_train_every_ticks: int = 1000
    continuous_train_window_ticks: int = 2000


class NeuralAlphaShadowSession:
    """
    Runs live neural alpha inference and publishes signals to shared memory
    so the C++ strategy can gate trades in real-time.
    """

    def __init__(self, cfg: ShadowSessionConfig) -> None:
        self.cfg = cfg
        self._device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
        self._model: CryptoAlphaNet | None = None
        self._secondary_model: CryptoAlphaNet | None = None
        # ring buffer: at least 500 ticks for rolling normalisation
        self._ring: list[dict] = []
        self._max_ring = max(500, cfg.seq_len * 2)
        self._log_fp = open(cfg.log_path, "a")
        self._publisher = _SignalPublisher(cfg.signal_file)
        self._regime_publisher = RegimeSignalPublisher(cfg.regime_signal_file)
        self._regime_artifact = None
        if Path(cfg.regime_model_path).exists():
            try:
                self._regime_artifact = load_regime_artifact(cfg.regime_model_path)
                print(f"Regime model loaded from {cfg.regime_model_path}")
            except Exception as exc:
                print(f"[WARN] failed to load regime model {cfg.regime_model_path}: {exc}")
        self._running = False
        self._signals: list[float] = []
        self._outcomes: list[float] = []
        self._registry = ChampionChallengerRegistry(cfg.registry_path)
        self._drift_guard = DriftGuard(
            window=cfg.drift_window,
            min_samples=cfg.drift_min_samples,
            ic_floor=cfg.drift_ic_floor,
        )
        self._safe_mode_ticks_remaining = 0
        self._bridge = CoreBridge()
        self._bridge.open()
        self._processed_ticks = 0
        self._last_continuous_train_tick = 0

    # ── Model loading / training ──────────────────────────────────────────────

    def _build_model(self, d_spatial: int = 64, d_temporal: int = 128,
                     n_temp_layers: int = 3) -> CryptoAlphaNet:
        return CryptoAlphaNet(
            d_spatial=d_spatial,
            d_temporal=d_temporal,
            n_temp_layers=n_temp_layers,
            seq_len=self.cfg.seq_len,
        ).to(self._device).eval()

    def load_model(self, path: str) -> None:
        model = self._build_model(self.cfg.d_spatial, self.cfg.d_temporal)
        state = torch.load(path, map_location=self._device, weights_only=True)
        model.load_state_dict(state)
        self._model = model
        print(f"Model loaded from {path}")

    def load_secondary_model(self, path: str) -> None:
        # Secondary model uses smaller architecture
        model = self._build_model(d_spatial=32, d_temporal=64, n_temp_layers=1)
        state = torch.load(path, map_location=self._device, weights_only=True)
        model.load_state_dict(state)
        self._secondary_model = model
        print(f"Secondary model loaded from {path}")

    def _collect_training_ticks(self, n_ticks: int) -> pl.DataFrame:
        df = collect_from_core_bridge(n_ticks=n_ticks, interval_ms=self.cfg.interval_ms)
        if df is not None and len(df) > 0:
            return df

        print("[WARN] core bridge unavailable for training; falling back to session collectors")
        interval_s = self.cfg.interval_ms / 1000.0
        rows: list[dict] = []
        while len(rows) < n_ticks:
            rows.extend(self._fetch_tick())
            if len(rows) >= n_ticks:
                break
            time.sleep(interval_s)

        if not rows:
            raise RuntimeError("No data collected for training — core bridge and collectors unavailable.")
        return pl.DataFrame(rows[:n_ticks]).sort("timestamp_ns")

    def train_on_recent(self, n_ticks: int) -> None:
        print(f"Collecting {n_ticks} ticks for in-place training...")
        df = self._collect_training_ticks(n_ticks)

        resume_state = None
        if self._model is not None:
            resume_state = {k: v.detach().cpu().clone() for k, v in self._model.state_dict().items()}

        tcfg = TrainerConfig(
            epochs=self.cfg.train_epochs,
            n_folds=2,
            seq_len=self.cfg.seq_len,
            d_spatial=self.cfg.d_spatial,
            d_temporal=self.cfg.d_temporal,
            resume_state_dict=resume_state,
        )
        fold_results = walk_forward_train(df, tcfg)
        if not fold_results:
            raise RuntimeError("Training produced no fold results — dataset too small.")

        best = min(fold_results, key=lambda f: f["metrics"].get("loss_total", 1e9))
        candidate_state = best["model_state"]

        incumbent_oos_mse: float | None = None
        challenger_oos_mse: float | None = None
        keep_challenger = True

        holdout_start = int(len(df) * 0.8)
        holdout_df = df[holdout_start:]
        if len(holdout_df) >= self.cfg.seq_len * 2:
            challenger_oos_mse = self._evaluate_state_on_holdout(candidate_state, holdout_df)
            if resume_state is not None:
                incumbent_oos_mse = self._evaluate_state_on_holdout(resume_state, holdout_df)
                keep_challenger = challenger_oos_mse <= incumbent_oos_mse
                if not keep_challenger:
                    candidate_state = resume_state
                    print(
                        "[MODEL_SELECT] challenger rejected on holdout "
                        f"(incumbent_mse={incumbent_oos_mse:.6e}, "
                        f"challenger_mse={challenger_oos_mse:.6e})"
                    )

        model = self._build_model(self.cfg.d_spatial, self.cfg.d_temporal)
        model.load_state_dict(candidate_state)
        self._model = model

        out_path = Path(self.cfg.model_path) if self.cfg.model_path else Path("models/neural_alpha_latest.pt")
        out_path.parent.mkdir(parents=True, exist_ok=True)
        tmp_out = out_path.with_suffix(out_path.suffix + ".tmp")
        torch.save(candidate_state, tmp_out)
        tmp_out.replace(out_path)

        meta = {
            "trained_at_ns": time.time_ns(),
            "train_ticks": n_ticks,
            "train_epochs": self.cfg.train_epochs,
            "seq_len": self.cfg.seq_len,
            "d_spatial": self.cfg.d_spatial,
            "d_temporal": self.cfg.d_temporal,
            "metrics": best.get("metrics", {}),
            "oos_holdout_mse": {
                "incumbent": incumbent_oos_mse,
                "challenger": challenger_oos_mse,
                "selected": "challenger" if keep_challenger else "incumbent",
            },
        }
        out_meta = out_path.with_suffix(".json")
        out_meta.write_text(json.dumps(meta, indent=2))

        print(
            "In-place training done. "
            f"Val loss: {best['metrics'].get('loss_total', 'n/a'):.4f} "
            f"saved={out_path}"
        )

    def _evaluate_state_on_holdout(self, state_dict: dict[str, torch.Tensor], holdout_df: pl.DataFrame) -> float:
        dataset = LOBDataset(holdout_df, DatasetConfig(seq_len=self.cfg.seq_len))
        if len(dataset) == 0:
            return float("inf")

        loader = DataLoader(dataset, batch_size=64, shuffle=False)
        model = self._build_model(self.cfg.d_spatial, self.cfg.d_temporal)
        model.load_state_dict(state_dict, strict=False)
        model.eval()

        sqerr = 0.0
        n = 0
        with torch.no_grad():
            for batch in loader:
                lob = batch["lob"].to(self._device)
                scalar = batch["scalar"].to(self._device)
                pred_mid = model(lob, scalar)["returns"][:, -1, 2]
                true_mid = batch["labels"][:, -1, 2].to(self._device)
                diff = pred_mid - true_mid
                sqerr += float((diff * diff).sum().item())
                n += int(diff.numel())

        return sqerr / max(n, 1)

    # ── Inference + publish ───────────────────────────────────────────────────

    def _infer(self) -> dict | None:
        if self._model is None or len(self._ring) < self.cfg.seq_len:
            return None

        df_ring = pl.DataFrame(self._ring)

        # Rolling z-score over full ring (up to 500 ticks), then take last seq_len
        lob_ring    = compute_lob_tensor(df_ring)          # (ring_len, N_LEVELS, 4)
        scalar_ring = compute_scalar_features(df_ring)     # (ring_len, D_SCALAR)
        scalar_norm = rolling_normalise(scalar_ring)       # (ring_len, D_SCALAR)

        lob_np    = lob_ring[-self.cfg.seq_len:]
        scalar_np = scalar_norm[-self.cfg.seq_len:]

        lob_t    = torch.from_numpy(lob_np).unsqueeze(0).to(self._device)
        scalar_t = torch.from_numpy(scalar_np).unsqueeze(0).to(self._device)

        with torch.no_grad():
            out = self._model(lob_t, scalar_t)

        ret  = out["returns"][0, -1].cpu().numpy()    # (4,)
        risk = float(out["risk"][0, -1].cpu().item())

        # Mid-horizon signal: index 2 = 100-tick return
        raw_signal_bps = float(ret[2]) * 1e4

        # Ensemble: average with secondary model if available
        if self._secondary_model is not None:
            with torch.no_grad():
                out2 = self._secondary_model(lob_t, scalar_t)
            ret2 = out2["returns"][0, -1].cpu().numpy()
            raw_signal_bps = (raw_signal_bps + float(ret2[2]) * 1e4) / 2.0

        signal_bps = raw_signal_bps

        # Direction-head gating: require dir confidence > 0.55
        dir_probs = torch.softmax(out["direction"][0, -1], dim=-1).cpu().numpy()
        if signal_bps > 0 and dir_probs[2] <= 0.55:
            signal_bps = 0.0
        elif signal_bps < 0 and dir_probs[0] <= 0.55:
            signal_bps = 0.0

        # 1-tick entry gate: 1-tick (index 0) and mid-horizon must agree
        ret_1tick_bps = float(ret[0]) * 1e4
        if signal_bps > 0 and ret_1tick_bps <= 0:
            signal_bps = 0.0
        elif signal_bps < 0 and ret_1tick_bps >= 0:
            signal_bps = 0.0

        if self._safe_mode_ticks_remaining > 0:
            signal_bps = 0.0
            self._safe_mode_ticks_remaining -= 1

        # Publish to shared memory — C++ reads on next book update (< 100 ns)
        self._publisher.publish(signal_bps, risk)

        regime_probs = {"p_calm": 1.0, "p_trending": 0.0, "p_shock": 0.0, "p_illiquid": 0.0}
        if self._regime_artifact is not None:
            try:
                regime_probs = infer_regime_probabilities(df_ring, self._regime_artifact)
            except Exception as exc:
                print(f"[WARN] regime inference failed: {exc}")
        self._regime_publisher.publish(
            regime_probs["p_calm"],
            regime_probs["p_trending"],
            regime_probs["p_shock"],
            regime_probs["p_illiquid"],
        )

        last = self._ring[-1]
        mid = (last.get("best_bid", last.get("bid_price_1", 0.0)) +
               last.get("best_ask", last.get("ask_price_1", 0.0))) / 2.0

        return {
            "timestamp_ns":   last["timestamp_ns"],
            "exchange":       last.get("exchange", "BINANCE"),
            "mid_price":      mid,
            "ret_1tick_bps":  ret_1tick_bps,
            "ret_10tick_bps": float(ret[1]) * 1e4,
            "ret_mid_bps":    signal_bps,
            "ret_long_bps":   float(ret[3]) * 1e4,
            "risk_score":     risk,
            "signal":         float(ret[2]),
            "dir_p_down":     float(dir_probs[0]),
            "dir_p_flat":     float(dir_probs[1]),
            "dir_p_up":       float(dir_probs[2]),
            "gated":          signal_bps == 0.0 and raw_signal_bps != 0.0,
            "safe_mode":      self._safe_mode_ticks_remaining > 0,
            "p_calm":         regime_probs["p_calm"],
            "p_trending":     regime_probs["p_trending"],
            "p_shock":        regime_probs["p_shock"],
            "p_illiquid":     regime_probs["p_illiquid"],
        }

    def _trigger_safe_mode(self, reason: str) -> None:
        self._safe_mode_ticks_remaining = max(self._safe_mode_ticks_remaining, self.cfg.safe_mode_ticks)
        rollback_path = self._registry.rollback_to_previous_champion(reason=reason)
        if rollback_path and Path(rollback_path).exists():
            try:
                self.load_model(rollback_path)
                print(f"[SAFE_MODE] rollback model loaded: {rollback_path}")
            except Exception as exc:
                print(f"[SAFE_MODE] rollback load failed: {exc}")
        else:
            print("[SAFE_MODE] no rollback champion available; publishing neutral signal only")

    # ── Data collection ───────────────────────────────────────────────────────

    def _fetch_tick(self) -> list[dict]:
        bridge_ticks = self._bridge.read_new_ticks()
        if bridge_ticks:
            return bridge_ticks

        ticks: list[dict] = []
        _fetchers = {
            "BINANCE": _fetch_binance_l5,
            "KRAKEN": _fetch_kraken_l5,
            "OKX": _fetch_okx_l5,
            "COINBASE": _fetch_coinbase_l5,
            "SOLANA": _fetch_solana_l5,
        }
        for ex in self.cfg.exchanges:
            fetcher = _fetchers.get(ex)
            if fetcher:
                row = fetcher(self.cfg.symbol)
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

    def _maybe_continuous_train(self) -> None:
        if self.cfg.continuous_train_every_ticks <= 0:
            return
        ticks_since_train = self._processed_ticks - self._last_continuous_train_tick
        if ticks_since_train < self.cfg.continuous_train_every_ticks:
            return

        train_window = max(self.cfg.seq_len * 4, self.cfg.continuous_train_window_ticks)
        print(
            f"[CONTINUOUS_TRAIN] starting incremental retrain at tick={self._processed_ticks} "
            f"window={train_window}"
        )
        try:
            self.train_on_recent(train_window)
            self._last_continuous_train_tick = self._processed_ticks
            print("[CONTINUOUS_TRAIN] completed")
        except Exception as exc:
            print(f"[CONTINUOUS_TRAIN] failed: {exc}")

    # ── Main loop ─────────────────────────────────────────────────────────────

    def run(self) -> None:
        self._running = True
        interval_s  = self.cfg.interval_ms / 1000.0
        end_time    = time.time() + self.cfg.duration_s
        last_report = time.time()
        prev_mid    = None

        print(
            f"Shadow session started — duration={self.cfg.duration_s}s  "
            f"interval={self.cfg.interval_ms}ms  symbol={self.cfg.symbol}  "
            f"exchanges={','.join(self.cfg.exchanges)}  signal_file={self.cfg.signal_file} "
            f"regime_signal_file={self.cfg.regime_signal_file}"
        )

        try:
            while self._running and time.time() < end_time:
                tick_start = time.time()

                ticks = self._fetch_tick()
                for tick in ticks:
                    self._ring.append(tick)
                self._processed_ticks += len(ticks)

                if len(self._ring) > self._max_ring:
                    self._ring = self._ring[-self._max_ring:]

                self._maybe_continuous_train()
                signal_info = self._infer()
                if signal_info:
                    if prev_mid is not None and prev_mid > 0:
                        realised = (signal_info["mid_price"] - prev_mid) / prev_mid
                        self._outcomes.append(realised)
                        drift_triggered = self._drift_guard.update(signal_info["signal"], realised)
                        if drift_triggered:
                            ic = self._drift_guard.current_ic()
                            self._trigger_safe_mode(
                                reason=f"drift_ic_breach ic={ic:.4f} floor={self.cfg.drift_ic_floor:.4f}"
                            )

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
            self._bridge.close()
            self._publisher.close()
            self._regime_publisher.close()
            print("Shadow session complete.")

    def stop(self) -> None:
        self._running = False


# ── CLI ───────────────────────────────────────────────────────────────────────

def main() -> None:
    ap = argparse.ArgumentParser(description="Neural alpha shadow session")
    ap.add_argument("--model-path",           type=str, default=None, dest="model_path")
    ap.add_argument("--secondary-model-path", type=str, default=None, dest="secondary_model_path")
    ap.add_argument("--log-path",             type=str, default="neural_alpha_shadow.jsonl",
                    dest="log_path")
    ap.add_argument("--signal-file",          type=str, default=_SIGNAL_FILE, dest="signal_file",
                    help="Shared memory file path read by C++ AlphaSignalReader")
    ap.add_argument("--regime-signal-file",   type=str, default=_REGIME_SIGNAL_FILE, dest="regime_signal_file",
                    help="Shared memory file path read by C++ RegimeSignalReader")
    ap.add_argument("--regime-model-path",    type=str, default="models/r2_regime_model.json", dest="regime_model_path",
                    help="Trained R2 regime artifact JSON for live regime inference")
    ap.add_argument("--interval-ms",          type=int, default=500, dest="interval_ms")
    ap.add_argument("--duration",             type=int, default=3600)
    ap.add_argument("--report-interval",      type=int, default=60, dest="report_interval_s")
    ap.add_argument("--seq-len",              type=int, default=64, dest="seq_len")
    ap.add_argument("--entry-bps",            type=float, default=5.0, dest="entry_threshold_bps")
    ap.add_argument("--d-spatial",            type=int, default=64, dest="d_spatial")
    ap.add_argument("--d-temporal",           type=int, default=128, dest="d_temporal")
    ap.add_argument("--train-ticks",          type=int, default=0, dest="train_ticks")
    ap.add_argument("--train-epochs",         type=int, default=10, dest="train_epochs")
    ap.add_argument("--symbol",               type=str, default="BTCUSDT")
    ap.add_argument("--exchanges",            type=str, default="BINANCE,KRAKEN,OKX,COINBASE")
    ap.add_argument("--registry-path",        type=str, default="models/model_registry.json", dest="registry_path")
    ap.add_argument("--drift-window",         type=int, default=200, dest="drift_window")
    ap.add_argument("--drift-min-samples",    type=int, default=60, dest="drift_min_samples")
    ap.add_argument("--drift-ic-floor",       type=float, default=-0.05, dest="drift_ic_floor")
    ap.add_argument("--safe-mode-ticks",      type=int, default=120, dest="safe_mode_ticks")
    ap.add_argument("--continuous-train-every-ticks", type=int, default=1000,
                    dest="continuous_train_every_ticks")
    ap.add_argument("--continuous-train-window-ticks", type=int, default=2000,
                    dest="continuous_train_window_ticks")
    args = ap.parse_args()

    cfg = ShadowSessionConfig(
        model_path=args.model_path,
        secondary_model_path=args.secondary_model_path,
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
        symbol=args.symbol,
        exchanges=args.exchanges.split(","),
        signal_file=args.signal_file,
        regime_signal_file=args.regime_signal_file,
        regime_model_path=args.regime_model_path,
        registry_path=args.registry_path,
        drift_window=args.drift_window,
        drift_min_samples=args.drift_min_samples,
        drift_ic_floor=args.drift_ic_floor,
        safe_mode_ticks=args.safe_mode_ticks,
        continuous_train_every_ticks=args.continuous_train_every_ticks,
        continuous_train_window_ticks=args.continuous_train_window_ticks,
    )

    session = NeuralAlphaShadowSession(cfg)

    def _handle_sigint(sig, frame):
        print("\nInterrupt received — stopping session...")
        session.stop()

    signal.signal(signal.SIGINT, _handle_sigint)

    if cfg.model_path and Path(cfg.model_path).exists():
        session.load_model(cfg.model_path)
        if cfg.secondary_model_path and Path(cfg.secondary_model_path).exists():
            session.load_secondary_model(cfg.secondary_model_path)
    elif cfg.train_ticks > 0:
        session.train_on_recent(cfg.train_ticks)
    else:
        print(
            "WARNING: no model loaded and --train-ticks not set. "
            "Continuous training will bootstrap once enough ticks accumulate."
        )

    session.run()


if __name__ == "__main__":
    main()
