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
    Writes to /ipc/neural_alpha_signal.bin (32 bytes, seqlock-protected):
        offset  0: uint64   seq         — seqlock counter (even=stable, odd=writing)
        offset  8: float64  signal_bps  — mid-horizon return prediction (bps)
        offset 16: float64  risk_score  — adverse-selection probability [0, 1]
        offset 24: int64    ts_ns       — nanosecond timestamp
    The C++ AlphaSignalReader (core/ipc/alpha_signal.hpp) mmaps this file.

Usage:
    python -m research.neural_alpha.shadow_session         --model-path models/neural_alpha_btcusdt_latest.pt         --secondary-model-path models/neural_alpha_btcusdt_secondary.pt         --duration 86400         --interval-ms 500         --symbol BTCUSDT         --exchanges BINANCE,KRAKEN,OKX,COINBASE
"""

from __future__ import annotations

import argparse
import json
import mmap
import os
import signal
import struct
import time
import warnings
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import numpy as np
import polars as pl
import torch
from torch.utils.data import DataLoader

from research.regime import (
    RegimeConfig,
    RegimeSignalPublisher,
    infer_regime_probabilities,
    load_regime_artifact,
    save_regime_artifact,
    train_regime_model_from_df,
)

from .core_bridge import RING_PATH, CoreBridge
from ..data.dataset import DatasetConfig, LOBDataset, rolling_normalise
from ..data.features import compute_lob_tensor, compute_scalar_features
from ..models.model import CryptoAlphaNet
from ..models.trainer import TrainerConfig, walk_forward_train
from ..operations.governance import ChampionChallengerRegistry, DriftGuard, EnsembleCanary
from ..pipeline import (
    _fetch_binance_l5,
    _fetch_coinbase_l5,
    _fetch_kraken_l5,
    _fetch_okx_l5,
    collect_from_core_bridge,
)

_SIGNAL_FILE = "/tmp/trt_ipc/neural_alpha_signal.bin"
_SIGNAL_SIZE = 32
_SEQ_FMT = "=Q"
_SEQ_OFFSET = 0
_DATA_FMT = "=ddq"
_DATA_OFFSET = 8
_REGIME_SIGNAL_FILE = "/tmp/trt_ipc/regime_signal.bin"
_MAX_EXCHANGE_JUMP_NS = 60 * 1000000000
_LEGACY_ALPHA_OPTIONAL_KEYS = frozenset({
    "spatial_enc.pool.0.weight",
    "spatial_enc.pool.0.bias",
    "spatial_enc.pool.1.weight",
    "spatial_enc.pool.1.bias",
    "fusion.3.weight",
    "fusion.3.bias",
})


def _load_model_state_with_compat(
    model: CryptoAlphaNet,
    state: dict[str, torch.Tensor],
    *,
    checkpoint_path: str,
    allow_legacy_missing: bool = False,
) -> None:
    incompatible = model.load_state_dict(state, strict=False)
    missing = set(incompatible.missing_keys)
    unexpected = set(incompatible.unexpected_keys)

    allowed_missing = _LEGACY_ALPHA_OPTIONAL_KEYS if allow_legacy_missing else frozenset()
    disallowed_missing = missing - allowed_missing
    if disallowed_missing or unexpected:
        details: list[str] = []
        if disallowed_missing:
            details.append(f"missing keys: {sorted(disallowed_missing)}")
        if unexpected:
            details.append(f"unexpected keys: {sorted(unexpected)}")
        raise RuntimeError(
            f"Checkpoint {checkpoint_path} is incompatible with {model.__class__.__name__}: "
            + "; ".join(details)
        )

    if missing:
        warnings.warn(
            f"Loaded legacy checkpoint {checkpoint_path} with default initialisers for missing keys: {sorted(missing)}",
            RuntimeWarning,
            stacklevel=2,
        )


@dataclass
class VenueRuntimeStats:
    ticks_received: int = 0
    ticks_used: int = 0
    missing_venue_incidents: int = 0
    rest_fallback_usage: int = 0
    resnapshot_count: int = 0
    startup_confirmed: bool = False
    consecutive_missing_polls: int = 0
    last_source: str = "none"


@dataclass
class ShadowSessionConfig:
    model_path: str | None = None
    secondary_model_path: str | None = None
    log_path: str = "neural_alpha_shadow.jsonl"
    interval_ms: int = 500
    duration_s: int = 3600
    report_interval_s: int = 60
    seq_len: int = 64
    d_spatial: int = 64
    d_temporal: int = 128
    train_ticks: int = 0
    train_epochs: int = 15
    symbol: str = "BTCUSDT"
    exchanges: list[str] = field(default_factory=lambda: ["BINANCE", "KRAKEN", "OKX", "COINBASE"])
    signal_file: str = _SIGNAL_FILE
    regime_signal_file: str = _REGIME_SIGNAL_FILE
    lob_feed_path: str = RING_PATH
    regime_model_path: str = "models/r2_regime_model.json"
    registry_path: str = "models/model_registry.json"
    drift_window: int = 200
    drift_min_samples: int = 60
    drift_ic_floor: float = -0.05
    safe_mode_ticks: int = 120
    continuous_train_every_ticks: int = 1000
    continuous_train_window_ticks: int = 2000
    canary_ic_margin: float = 0.02
    canary_icir_floor: float = 0.0
    canary_window: int = 200
    canary_min_samples: int = 60
    require_full_model_stack: bool = True


class _SignalPublisher:
    def __init__(self, path: str = _SIGNAL_FILE) -> None:
        self._path = path
        _ensure_parent_dir(path)
        self._f = open(path, "w+b")
        self._f.write(b"\x00" * _SIGNAL_SIZE)
        self._f.flush()
        self._mm = mmap.mmap(self._f.fileno(), _SIGNAL_SIZE)

    def publish(self, signal_bps: float, risk_score: float) -> None:
        ts_ns = time.time_ns()
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


def _ensure_parent_dir(path: str | Path) -> None:
    Path(path).expanduser().resolve().parent.mkdir(parents=True, exist_ok=True)


def _symbol_model_path(symbol: str, variant: str = "latest") -> Path:
    symbol_tag = symbol.lower()
    return Path(f"models/neural_alpha_{symbol_tag}_{variant}.pt")


def _extract_tick_timestamps(tick: dict[str, Any]) -> tuple[int, int]:
    exchange_ts = int(
        tick.get("timestamp_exchange_ns", tick.get("exchange_timestamp_ns", tick.get("timestamp_ns", 0)))
    )
    local_ts = int(
        tick.get("timestamp_local_ns", tick.get("local_timestamp_ns", tick.get("timestamp_ns", 0)))
    )
    return (exchange_ts, local_ts)


def _build_signal_alignment(records: list[dict[str, Any]]) -> tuple[np.ndarray, np.ndarray]:
    ordered = sorted(records, key=lambda r: (int(r.get("event_index", 0)), int(r.get("session_elapsed_ns", 0))))
    signals: list[float] = []
    outcomes: list[float] = []
    for i in range(len(ordered) - 1):
        current = ordered[i]
        following = ordered[i + 1]
        current_mid = float(current.get("mid_price", 0.0))
        next_mid = float(following.get("mid_price", 0.0))
        if current_mid <= 0.0 or next_mid <= 0.0:
            continue
        signals.append(float(current.get("signal", 0.0)))
        outcomes.append((next_mid - current_mid) / current_mid)
    return (np.asarray(signals, dtype=np.float64), np.asarray(outcomes, dtype=np.float64))


def _extract_signal_series(records: list[dict[str, Any]]) -> tuple[np.ndarray, np.ndarray]:
    ordered = sorted(records, key=lambda r: (int(r.get("event_index", 0)), int(r.get("session_elapsed_ns", 0))))
    raw_signals_bps = np.array(
        [float(record.get("signal", 0.0)) * 10000.0 for record in ordered],
        dtype=np.float64,
    )
    effective_signals_bps = np.array(
        [float(record.get("ret_mid_bps", 0.0)) for record in ordered],
        dtype=np.float64,
    )
    return (raw_signals_bps, effective_signals_bps)


def _summarise_timestamp_quality(records: list[dict[str, Any]]) -> dict[str, int | float | bool]:
    diagnostics: dict[str, int | float | bool] = {
        "records": len(records),
        "exchange_missing": 0,
        "local_missing": 0,
        "exchange_non_monotonic": 0,
        "local_non_monotonic": 0,
        "event_index_non_monotonic": 0,
        "cross_venue_exchange_jumps": 0,
        "max_exchange_jump_ns": 0,
        "max_local_gap_ns": 0,
        "exchange_monotonic": True,
        "local_monotonic": True,
        "event_index_monotonic": True,
    }
    prev_exchange = None
    prev_local = None
    prev_event_index = None
    prev_venue_exchange_ts: dict[str, int] = {}
    for record in sorted(records, key=lambda r: (int(r.get("event_index", 0)), int(r.get("session_elapsed_ns", 0)))):
        exchange_ts = int(record.get("timestamp_exchange_ns", 0))
        local_ts = int(record.get("timestamp_local_ns", 0))
        event_index = int(record.get("event_index", 0))
        venue = str(record.get("exchange", "UNKNOWN"))
        if exchange_ts <= 0:
            diagnostics["exchange_missing"] = int(diagnostics["exchange_missing"]) + 1
        if local_ts <= 0:
            diagnostics["local_missing"] = int(diagnostics["local_missing"]) + 1
        if prev_exchange is not None and exchange_ts > 0:
            diagnostics["max_exchange_jump_ns"] = max(int(diagnostics["max_exchange_jump_ns"]), abs(exchange_ts - prev_exchange))
            if exchange_ts < prev_exchange:
                diagnostics["exchange_non_monotonic"] = int(diagnostics["exchange_non_monotonic"]) + 1
                diagnostics["exchange_monotonic"] = False
        if prev_local is not None and local_ts > 0:
            diagnostics["max_local_gap_ns"] = max(int(diagnostics["max_local_gap_ns"]), abs(local_ts - prev_local))
            if local_ts < prev_local:
                diagnostics["local_non_monotonic"] = int(diagnostics["local_non_monotonic"]) + 1
                diagnostics["local_monotonic"] = False
        if prev_event_index is not None and event_index <= prev_event_index:
            diagnostics["event_index_non_monotonic"] = int(diagnostics["event_index_non_monotonic"]) + 1
            diagnostics["event_index_monotonic"] = False
        previous_venue_ts = prev_venue_exchange_ts.get(venue)
        if previous_venue_ts is not None and exchange_ts > 0 and previous_venue_ts > 0:
            jump = abs(exchange_ts - previous_venue_ts)
            diagnostics["max_exchange_jump_ns"] = max(int(diagnostics["max_exchange_jump_ns"]), jump)
            if jump > _MAX_EXCHANGE_JUMP_NS:
                diagnostics["cross_venue_exchange_jumps"] = int(diagnostics["cross_venue_exchange_jumps"]) + 1
        if exchange_ts > 0:
            prev_venue_exchange_ts[venue] = exchange_ts
            prev_exchange = exchange_ts
        if local_ts > 0:
            prev_local = local_ts
        prev_event_index = event_index
    diagnostics["has_timestamp_issues"] = any(
        int(diagnostics[key]) > 0
        for key in (
            "exchange_missing",
            "local_missing",
            "exchange_non_monotonic",
            "local_non_monotonic",
            "event_index_non_monotonic",
            "cross_venue_exchange_jumps",
        )
    )
    return diagnostics


class NeuralAlphaShadowSession:
    def __init__(self, cfg: ShadowSessionConfig) -> None:
        self.cfg = cfg
        self._device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
        self._model: CryptoAlphaNet | None = None
        self._secondary_model: CryptoAlphaNet | None = None
        self._ring: list[dict[str, Any]] = []
        self._max_ring = max(500, cfg.seq_len * 2)
        _ensure_parent_dir(cfg.log_path)
        self._log_fp = open(cfg.log_path, "a", encoding="utf-8")
        self._publisher = _SignalPublisher(cfg.signal_file)
        _ensure_parent_dir(cfg.regime_signal_file)
        self._regime_publisher = RegimeSignalPublisher(cfg.regime_signal_file)
        self._regime_artifact = None
        if Path(cfg.regime_model_path).exists():
            try:
                self._regime_artifact = load_regime_artifact(cfg.regime_model_path)
            except Exception:
                self._regime_artifact = None
        self._running = False
        self._signal_records: list[dict[str, Any]] = []
        self._registry = ChampionChallengerRegistry(cfg.registry_path)
        self._drift_guard = DriftGuard(window=cfg.drift_window, min_samples=cfg.drift_min_samples, ic_floor=cfg.drift_ic_floor)
        self._safe_mode_ticks_remaining = 0
        self._safe_mode_reason: str | None = None
        self._bridge = CoreBridge(cfg.lob_feed_path)
        self._bridge.open()
        self._processed_ticks = 0
        self._last_continuous_train_tick = 0
        self._session_event_index = 0
        self._session_wall_start_ns = time.time_ns()
        self._session_steady_start_ns = time.monotonic_ns()
        self._canary: EnsembleCanary | None = None
        self._prev_primary_signal: float | None = None
        self._prev_ensemble_signal: float | None = None
        self._venue_stats = {exchange.upper(): VenueRuntimeStats() for exchange in cfg.exchanges}
        self._gating_reason_counts = {"confidence_gate": 0, "horizon_disagreement_gate": 0, "safe_mode_gate": 0}

    def _build_model(self, d_spatial: int = 64, d_temporal: int = 128, n_temp_layers: int = 3) -> CryptoAlphaNet:
        return CryptoAlphaNet(
            d_spatial=d_spatial,
            d_temporal=d_temporal,
            n_temp_layers=n_temp_layers,
            seq_len=self.cfg.seq_len,
        ).to(self._device).eval()

    def load_model(self, path: str) -> None:
        model = self._build_model(self.cfg.d_spatial, self.cfg.d_temporal)
        state = torch.load(path, map_location=self._device, weights_only=True)
        _load_model_state_with_compat(model, state, checkpoint_path=path)
        self._model = model

    def load_secondary_model(self, path: str) -> None:
        model = self._build_model(d_spatial=32, d_temporal=64, n_temp_layers=1)
        state = torch.load(path, map_location=self._device, weights_only=True)
        _load_model_state_with_compat(
            model,
            state,
            checkpoint_path=path,
            allow_legacy_missing=True,
        )
        self._secondary_model = model
        self._canary = EnsembleCanary(
            window=self.cfg.canary_window,
            min_samples=self.cfg.canary_min_samples,
            ic_margin=self.cfg.canary_ic_margin,
            icir_floor=self.cfg.canary_icir_floor,
        )
        self._prev_primary_signal = None
        self._prev_ensemble_signal = None

    def _validate_production_stack(self) -> None:
        missing: list[str] = []
        if self._model is None:
            missing.append("primary alpha model")
        if self._secondary_model is None:
            missing.append("secondary alpha model")
        if self._regime_artifact is None:
            missing.append("regime model")
        if missing and self.cfg.require_full_model_stack:
            raise RuntimeError(
                "Research production stack requires "
                + ", ".join(missing)
                + ". Provide the saved artifacts or start with --no-require-full-model-stack only for non-production debugging."
            )

    def _unload_secondary_model(self, reason: str) -> None:
        self._secondary_model = None
        self._canary = None
        self._prev_primary_signal = None
        self._prev_ensemble_signal = None
        self._trigger_safe_mode(reason=f"ensemble_canary: {reason}")

    def _collect_training_ticks(self, n_ticks: int) -> pl.DataFrame:
        df = collect_from_core_bridge(n_ticks=n_ticks, interval_ms=self.cfg.interval_ms)
        if df is not None and len(df) > 0:
            return df
        interval_s = self.cfg.interval_ms / 1000.0
        rows: list[dict[str, Any]] = []
        while len(rows) < n_ticks:
            rows.extend(self._fetch_tick())
            if len(rows) >= n_ticks:
                break
            time.sleep(interval_s)
        if not rows:
            raise RuntimeError("No data collected for training — core bridge and collectors unavailable.")
        return pl.DataFrame(rows[:n_ticks]).sort("timestamp_ns")

    def train_on_recent(self, n_ticks: int) -> None:
        df = self._collect_training_ticks(n_ticks)
        resume_state = None
        if self._model is not None:
            resume_state = {k: v.detach().cpu().clone() for (k, v) in self._model.state_dict().items()}
        max_folds = max(1, len(df) // (4 * self.cfg.seq_len))
        n_folds = min(2, max_folds)
        tcfg = TrainerConfig(
            epochs=self.cfg.train_epochs,
            n_folds=n_folds,
            seq_len=self.cfg.seq_len,
            d_spatial=self.cfg.d_spatial,
            d_temporal=self.cfg.d_temporal,
            resume_state_dict=resume_state,
            lr_warmup_epochs=min(3, self.cfg.train_epochs // 4),
            early_stop_patience=4,
            log_every_epochs=1,
        )
        fold_results = walk_forward_train(df, tcfg)
        if not fold_results:
            return
        best = min(fold_results, key=lambda f: f["metrics"].get("loss_total", 1000000000.0))
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
        model = self._build_model(self.cfg.d_spatial, self.cfg.d_temporal)
        model.load_state_dict(candidate_state)
        self._model = model
        out_path = Path(self.cfg.model_path) if self.cfg.model_path else _symbol_model_path(self.cfg.symbol)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        tmp_out = out_path.with_suffix(out_path.suffix + ".tmp")
        torch.save(candidate_state, tmp_out)
        tmp_out.replace(out_path)
        selected_metrics = dict(best.get("metrics", {}))
        meta = {
            "trained_at_ns": time.time_ns(),
            "train_ticks": n_ticks,
            "train_epochs": self.cfg.train_epochs,
            "seq_len": self.cfg.seq_len,
            "d_spatial": self.cfg.d_spatial,
            "d_temporal": self.cfg.d_temporal,
            "metrics": selected_metrics,
            "oos_holdout_mse": {
                "incumbent": incumbent_oos_mse,
                "challenger": challenger_oos_mse,
                "selected": "challenger" if keep_challenger else "incumbent",
            },
        }
        out_meta = out_path.with_suffix(".json")
        out_meta.write_text(json.dumps(meta, indent=2), encoding="utf-8")
        secondary_path = Path(self.cfg.secondary_model_path) if self.cfg.secondary_model_path else _symbol_model_path(self.cfg.symbol, "secondary")
        try:
            self._train_secondary_on_data(df, secondary_path)
        except Exception:
            pass
        try:
            self._train_regime_on_data(df)
        except Exception:
            pass

    def _train_secondary_on_data(self, df: pl.DataFrame, out_path: Path) -> None:
        max_folds = max(1, len(df) // (4 * self.cfg.seq_len))
        n_folds = min(2, max_folds)
        tcfg = TrainerConfig(
            epochs=self.cfg.train_epochs,
            n_folds=n_folds,
            seq_len=self.cfg.seq_len,
            d_spatial=32,
            d_temporal=64,
            n_temp_layers=1,
            dropout=0.25,
            lr=0.0007,
            w_return=0.2,
            w_direction=1.0,
            w_risk=0.7,
            fold_seed_offset=9999,
            lr_warmup_epochs=min(3, self.cfg.train_epochs // 4),
            early_stop_patience=4,
            log_every_epochs=1,
        )
        fold_results = walk_forward_train(df, tcfg)
        if not fold_results:
            return
        best = min(fold_results, key=lambda f: f["metrics"].get("loss_total", 1000000000.0))
        state = best["model_state"]
        out_path.parent.mkdir(parents=True, exist_ok=True)
        tmp_out = out_path.with_suffix(out_path.suffix + ".tmp")
        torch.save(state, tmp_out)
        tmp_out.replace(out_path)
        model = self._build_model(d_spatial=32, d_temporal=64, n_temp_layers=1)
        model.load_state_dict(state)
        self._secondary_model = model
        self._canary = EnsembleCanary(
            window=self.cfg.canary_window,
            min_samples=self.cfg.canary_min_samples,
            ic_margin=self.cfg.canary_ic_margin,
            icir_floor=self.cfg.canary_icir_floor,
        )
        self._prev_primary_signal = None
        self._prev_ensemble_signal = None
        selected_metrics = dict(best.get("metrics", {}))

    def _train_regime_on_data(self, df: pl.DataFrame) -> None:
        regime_path = Path(self.cfg.regime_model_path)
        (artifact, distribution) = train_regime_model_from_df(df, RegimeConfig())
        regime_path.parent.mkdir(parents=True, exist_ok=True)
        save_regime_artifact(artifact, str(regime_path))
        self._regime_artifact = artifact
    def _evaluate_state_on_holdout(
        self, state_dict: dict[str, torch.Tensor], holdout_df: pl.DataFrame
    ) -> float:
        dataset = LOBDataset(holdout_df, DatasetConfig(seq_len=self.cfg.seq_len))
        if len(dataset) == 0:
            return float("inf")
        loader = DataLoader(dataset, batch_size=64, shuffle=False)
        model = self._build_model(self.cfg.d_spatial, self.cfg.d_temporal)
        try:
            model.load_state_dict(state_dict, strict=True)
        except RuntimeError:
            return float("inf")
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

    def _infer(self) -> dict[str, Any] | None:
        if self._model is None or len(self._ring) < self.cfg.seq_len:
            return None
        df_ring = pl.DataFrame(self._ring)
        lob_ring = compute_lob_tensor(df_ring)
        scalar_ring = compute_scalar_features(df_ring)
        scalar_norm = rolling_normalise(scalar_ring)
        lob_np = lob_ring[-self.cfg.seq_len :]
        scalar_np = scalar_norm[-self.cfg.seq_len :]
        lob_t = torch.from_numpy(lob_np).unsqueeze(0).to(self._device)
        scalar_t = torch.from_numpy(scalar_np).unsqueeze(0).to(self._device)
        with torch.no_grad():
            out = self._model(lob_t, scalar_t)
        ret = out["returns"][0, -1].cpu().numpy()
        risk = float(out["risk"][0, -1].cpu().item())
        primary_signal = float(ret[2])
        raw_signal_bps = primary_signal * 10000.0
        if self._secondary_model is not None:
            with torch.no_grad():
                out2 = self._secondary_model(lob_t, scalar_t)
            ret2 = out2["returns"][0, -1].cpu().numpy()
            raw_signal_bps = (raw_signal_bps + float(ret2[2]) * 10000.0) / 2.0
        ensemble_signal = raw_signal_bps * 0.0001
        signal_bps = raw_signal_bps
        gating_reasons: list[str] = []
        dir_probs = torch.softmax(out["direction"][0, -1], dim=-1).cpu().numpy()
        if signal_bps > 0 and dir_probs[2] <= 0.55:
            signal_bps = 0.0
            gating_reasons.append("confidence_gate")
        elif signal_bps < 0 and dir_probs[0] <= 0.55:
            signal_bps = 0.0
            gating_reasons.append("confidence_gate")
        ret_1tick_bps = float(ret[0]) * 10000.0
        if signal_bps > 0 and ret_1tick_bps <= 0:
            signal_bps = 0.0
            gating_reasons.append("horizon_disagreement_gate")
        elif signal_bps < 0 and ret_1tick_bps >= 0:
            signal_bps = 0.0
            gating_reasons.append("horizon_disagreement_gate")
        safe_mode_active = self._safe_mode_ticks_remaining > 0
        if safe_mode_active:
            signal_bps = 0.0
            self._safe_mode_ticks_remaining -= 1
            gating_reasons.append("safe_mode_gate")
        for reason in set(gating_reasons):
            self._gating_reason_counts[reason] += 1
        self._publisher.publish(signal_bps, risk)
        regime_probs = {"p_calm": 1.0, "p_trending": 0.0, "p_shock": 0.0, "p_illiquid": 0.0}
        if self._regime_artifact is not None:
            try:
                regime_probs = infer_regime_probabilities(df_ring, self._regime_artifact)
            except Exception:
                pass
        self._regime_publisher.publish(
            regime_probs["p_calm"],
            regime_probs["p_trending"],
            regime_probs["p_shock"],
            regime_probs["p_illiquid"],
        )
        last = self._ring[-1]
        exchange_ts, local_ts = _extract_tick_timestamps(last)
        self._session_event_index += 1
        mid = (last.get("best_bid", last.get("bid_price_1", 0.0)) + last.get("best_ask", last.get("ask_price_1", 0.0))) / 2.0
        return {
            "timestamp_ns": local_ts,
            "timestamp_exchange_ns": exchange_ts,
            "timestamp_local_ns": local_ts,
            "event_index": self._session_event_index,
            "session_wall_start_ns": self._session_wall_start_ns,
            "session_elapsed_ns": time.monotonic_ns() - self._session_steady_start_ns,
            "exchange": last.get("exchange", "BINANCE"),
            "mid_price": mid,
            "ret_1tick_bps": ret_1tick_bps,
            "ret_10tick_bps": float(ret[1]) * 10000.0,
            "ret_mid_bps": signal_bps,
            "ret_long_bps": float(ret[3]) * 10000.0,
            "risk_score": risk,
            "signal": float(ret[2]),
            "dir_p_down": float(dir_probs[0]),
            "dir_p_flat": float(dir_probs[1]),
            "dir_p_up": float(dir_probs[2]),
            "gated": signal_bps == 0.0 and raw_signal_bps != 0.0,
            "safe_mode": safe_mode_active,
            "safe_mode_reason": self._safe_mode_reason if safe_mode_active else None,
            "gating_reasons": gating_reasons,
            "primary_signal": primary_signal,
            "ensemble_signal": ensemble_signal,
            "p_calm": regime_probs["p_calm"],
            "p_trending": regime_probs["p_trending"],
            "p_shock": regime_probs["p_shock"],
            "p_illiquid": regime_probs["p_illiquid"],
        }

    def _trigger_safe_mode(self, reason: str) -> None:
        self._safe_mode_ticks_remaining = max(self._safe_mode_ticks_remaining, self.cfg.safe_mode_ticks)
        self._safe_mode_reason = reason
        rollback_path = self._registry.rollback_to_previous_champion(reason=reason)
        if rollback_path and Path(rollback_path).exists():
            try:
                self.load_model(rollback_path)
            except Exception:
                pass

    def _fetch_tick(self) -> list[dict[str, Any]]:
        bridge_ticks = self._bridge.read_new_ticks()
        fetchers = {
            "BINANCE": _fetch_binance_l5,
            "KRAKEN": _fetch_kraken_l5,
            "OKX": _fetch_okx_l5,
            "COINBASE": _fetch_coinbase_l5,
        }
        grouped_ticks: dict[str, list[dict[str, Any]]] = {exchange: [] for exchange in self._venue_stats}
        for tick in bridge_ticks:
            exchange = str(tick.get("exchange", "UNKNOWN")).upper()
            if exchange in grouped_ticks:
                grouped_ticks[exchange].append(tick)
        rest_ticks: list[dict[str, Any]] = []
        for exchange, stats in self._venue_stats.items():
            venue_bridge_ticks = grouped_ticks.get(exchange, [])
            if venue_bridge_ticks:
                stats.ticks_received += len(venue_bridge_ticks)
                stats.startup_confirmed = True
                stats.consecutive_missing_polls = 0
                stats.last_source = "bridge"
                continue
            stats.missing_venue_incidents += 1
            stats.consecutive_missing_polls += 1
            if not stats.startup_confirmed:
                stats.last_source = "bridge_unavailable"
            else:
                stats.resnapshot_count += 1
                stats.last_source = "bridge_resnapshot"
            fetcher = fetchers.get(exchange)
            if fetcher is None:
                continue
            row = fetcher(self.cfg.symbol)
            if row:
                row = {**row, "exchange": exchange, "tick_source": "rest_fallback"}
                stats.rest_fallback_usage += 1
                stats.last_source = "rest_fallback"
                rest_ticks.append(row)
        if bridge_ticks or rest_ticks:
            return bridge_ticks + rest_ticks
        for exchange in self._venue_stats:
            fetcher = fetchers.get(exchange)
            if fetcher is None:
                continue
            row = fetcher(self.cfg.symbol)
            if row:
                row = {**row, "exchange": exchange, "tick_source": "rest_fallback"}
                stats = self._venue_stats[exchange]
                stats.rest_fallback_usage += 1
                stats.last_source = "rest_fallback"
                rest_ticks.append(row)
        return rest_ticks

    def _log(self, signal_info: dict[str, Any]) -> None:
        exchange = str(signal_info.get("exchange", "UNKNOWN")).upper()
        if exchange in self._venue_stats:
            self._venue_stats[exchange].ticks_used += 1
        self._signal_records.append(signal_info)
        self._log_fp.write(json.dumps(signal_info) + "\n")
        self._log_fp.flush()

    def _print_summary(self) -> None:
        n = len(self._signal_records)
        if n == 0:
            print("  [shadow] No signals yet.")
            return
        raw_sigs_bps, effective_sigs_bps = _extract_signal_series(self._signal_records)
        mean_sig = float(np.mean(effective_sigs_bps))
        std_sig = float(np.std(effective_sigs_bps))
        raw_mean_sig = float(np.mean(raw_sigs_bps))
        ic = 0.0
        icir = 0.0
        sig_aligned, out_aligned = _build_signal_alignment(self._signal_records)
        if len(out_aligned) >= 10 and out_aligned.std() > 0 and sig_aligned.std() > 0:
            ic = float(np.corrcoef(sig_aligned, out_aligned)[0, 1])
            if len(out_aligned) >= 40:
                aligned = len(out_aligned)
                chunk = aligned // 4
                ic_chunks = []
                for i in range(4):
                    s_c = sig_aligned[i * chunk : (i + 1) * chunk]
                    o_c = out_aligned[i * chunk : (i + 1) * chunk]
                    if s_c.std() > 1e-9 and o_c.std() > 1e-9:
                        ic_chunks.append(float(np.corrcoef(s_c, o_c)[0, 1]))
                if len(ic_chunks) >= 2:
                    ic_arr = np.array(ic_chunks)
                    if ic_arr.std() > 1e-9:
                        icir = float(ic_arr.mean() / ic_arr.std() * np.sqrt(252))
        diagnostics = _summarise_timestamp_quality(self._signal_records)
        gating = self._gating_reason_counts
        print(
            f"[Shadow] ticks={n}  mean_effective={mean_sig:.2f}bps"
            f"  mean_raw={raw_mean_sig:.2f}bps  std_effective={std_sig:.2f}bps"
            f"  IC={ic:.4f}  ICIR={icir:.3f}"
            f"  confidence_gate={gating['confidence_gate']}"
            f"  horizon_gate={gating['horizon_disagreement_gate']}"
            f"  safe_mode_gate={gating['safe_mode_gate']}"
            f"  ts_issues={int(diagnostics['has_timestamp_issues'])}"
        )

    def _maybe_continuous_train(self) -> None:
        if self.cfg.continuous_train_every_ticks <= 0:
            return
        ticks_since_train = self._processed_ticks - self._last_continuous_train_tick
        if ticks_since_train < self.cfg.continuous_train_every_ticks:
            return
        train_window = max(self.cfg.seq_len * 4, self.cfg.continuous_train_window_ticks)
        try:
            print(
                f"[Shadow] continuous retrain triggered processed_ticks={self._processed_ticks}"
                f" window_ticks={train_window}"
            )
            self.train_on_recent(train_window)
            self._last_continuous_train_tick = self._processed_ticks
            print(f"[Shadow] continuous retrain completed processed_ticks={self._processed_ticks}")
        except Exception as exc:
            print(
                f"[Shadow] continuous retrain failed processed_ticks={self._processed_ticks}: {exc}"
            )

    def run(self) -> None:
        self._running = True
        interval_s = self.cfg.interval_ms / 1000.0
        end_time = time.time() + self.cfg.duration_s
        last_report = time.time()
        try:
            while self._running and time.time() < end_time:
                tick_start = time.time()
                ticks = self._fetch_tick()
                for tick in ticks:
                    self._ring.append(tick)
                self._processed_ticks += len(ticks)
                if len(self._ring) > self._max_ring:
                    self._ring = self._ring[-self._max_ring :]
                self._maybe_continuous_train()
                signal_info = self._infer()
                if signal_info is None:
                    self._publisher.publish(0.0, 0.0)
                if signal_info:
                    if self._signal_records:
                        previous_record = self._signal_records[-1]
                        previous_mid = float(previous_record.get("mid_price", 0.0))
                        if previous_mid > 0:
                            realised = (float(signal_info["mid_price"]) - previous_mid) / previous_mid
                            previous_signal = float(previous_record.get("signal", 0.0))
                            drift_triggered = self._drift_guard.update(previous_signal, realised)
                            if drift_triggered:
                                ic = self._drift_guard.current_ic()
                                reason = f"drift_ic_breach ic={ic:.4f} floor={self.cfg.drift_ic_floor:.4f}"
                                self._trigger_safe_mode(reason=reason)
                            if self._canary is not None and self._prev_primary_signal is not None and self._prev_ensemble_signal is not None:
                                canary_triggered = self._canary.update(self._prev_primary_signal, self._prev_ensemble_signal, realised)
                                if canary_triggered:
                                    e_ic = self._canary.ensemble_ic()
                                    p_ic = self._canary.primary_ic()
                                    self._unload_secondary_model(
                                        reason=f"ic_degradation ensemble_ic={e_ic:.4f} primary_ic={p_ic:.4f} margin={self.cfg.canary_ic_margin}"
                                    )
                    self._prev_primary_signal = float(signal_info["primary_signal"])
                    self._prev_ensemble_signal = float(signal_info["ensemble_signal"])
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


def main() -> None:
    ap = argparse.ArgumentParser(description="Neural alpha shadow session")
    ap.add_argument("--model-path", type=str, default=None, dest="model_path")
    ap.add_argument("--secondary-model-path", type=str, default=None, dest="secondary_model_path")
    ap.add_argument("--log-path", type=str, default="neural_alpha_shadow.jsonl", dest="log_path")
    ap.add_argument("--signal-file", type=str, default=_SIGNAL_FILE, dest="signal_file", help="Shared memory file path read by C++ AlphaSignalReader")
    ap.add_argument("--lob-feed-path", type=str, default=RING_PATH, dest="lob_feed_path", help="Path to C++ LOB feed ring buffer")
    ap.add_argument("--regime-signal-file", type=str, default=_REGIME_SIGNAL_FILE, dest="regime_signal_file", help="Shared memory file path read by C++ RegimeSignalReader")
    ap.add_argument("--regime-model-path", type=str, default="models/r2_regime_model.json", dest="regime_model_path", help="Trained R2 regime artifact JSON for live regime inference")
    ap.add_argument("--interval-ms", type=int, default=500, dest="interval_ms")
    ap.add_argument("--duration", type=int, default=3600)
    ap.add_argument("--report-interval", type=int, default=60, dest="report_interval_s")
    ap.add_argument("--seq-len", type=int, default=64, dest="seq_len")
    ap.add_argument("--d-spatial", type=int, default=64, dest="d_spatial")
    ap.add_argument("--d-temporal", type=int, default=128, dest="d_temporal")
    ap.add_argument("--train-ticks", type=int, default=0, dest="train_ticks")
    ap.add_argument("--train-epochs", type=int, default=10, dest="train_epochs")
    ap.add_argument("--symbol", type=str, default="BTCUSDT")
    ap.add_argument("--exchanges", type=str, default="BINANCE,KRAKEN,OKX,COINBASE")
    ap.add_argument("--registry-path", type=str, default="models/model_registry.json", dest="registry_path")
    ap.add_argument("--drift-window", type=int, default=200, dest="drift_window")
    ap.add_argument("--drift-min-samples", type=int, default=60, dest="drift_min_samples")
    ap.add_argument("--drift-ic-floor", type=float, default=-0.05, dest="drift_ic_floor")
    ap.add_argument("--safe-mode-ticks", type=int, default=120, dest="safe_mode_ticks")
    ap.add_argument("--continuous-train-every-ticks", type=int, default=1000, dest="continuous_train_every_ticks")
    ap.add_argument("--continuous-train-window-ticks", type=int, default=2000, dest="continuous_train_window_ticks")
    ap.add_argument("--canary-ic-margin", type=float, default=0.02, dest="canary_ic_margin", help="Max IC degradation of ensemble vs primary before canary fires")
    ap.add_argument("--canary-icir-floor", type=float, default=0.0, dest="canary_icir_floor", help="Min ensemble ICIR before canary fires")
    ap.add_argument("--canary-window", type=int, default=200, dest="canary_window")
    ap.add_argument("--canary-min-samples", type=int, default=60, dest="canary_min_samples")
    ap.add_argument("--require-full-model-stack", action=argparse.BooleanOptionalAction, default=True, dest="require_full_model_stack", help="Require primary + secondary alpha models and regime artifact before entering production shadow mode.")
    args = ap.parse_args()
    cfg = ShadowSessionConfig(
        model_path=args.model_path,
        secondary_model_path=args.secondary_model_path,
        log_path=args.log_path,
        interval_ms=args.interval_ms,
        duration_s=args.duration,
        report_interval_s=args.report_interval_s,
        seq_len=args.seq_len,
        d_spatial=args.d_spatial,
        d_temporal=args.d_temporal,
        train_ticks=args.train_ticks,
        train_epochs=args.train_epochs,
        symbol=args.symbol,
        exchanges=[exchange.strip().upper() for exchange in args.exchanges.split(",") if exchange.strip()],
        signal_file=args.signal_file,
        lob_feed_path=args.lob_feed_path,
        regime_signal_file=args.regime_signal_file,
        regime_model_path=args.regime_model_path,
        registry_path=args.registry_path,
        drift_window=args.drift_window,
        drift_min_samples=args.drift_min_samples,
        drift_ic_floor=args.drift_ic_floor,
        safe_mode_ticks=args.safe_mode_ticks,
        continuous_train_every_ticks=args.continuous_train_every_ticks,
        continuous_train_window_ticks=args.continuous_train_window_ticks,
        canary_ic_margin=args.canary_ic_margin,
        canary_icir_floor=args.canary_icir_floor,
        canary_window=args.canary_window,
        canary_min_samples=args.canary_min_samples,
        require_full_model_stack=args.require_full_model_stack,
    )
    session = NeuralAlphaShadowSession(cfg)

    def _handle_sigint(sig: int, frame: object) -> None:
        print("\nInterrupt received — stopping session...")
        session.stop()

    signal.signal(signal.SIGINT, _handle_sigint)
    primary_model_path = Path(cfg.model_path) if cfg.model_path else _symbol_model_path(cfg.symbol, "latest")
    secondary_model_path = Path(cfg.secondary_model_path) if cfg.secondary_model_path else _symbol_model_path(cfg.symbol, "secondary")
    if primary_model_path.exists():
        session.load_model(str(primary_model_path))
    if secondary_model_path.exists():
        session.load_secondary_model(str(secondary_model_path))
    any_missing = session._model is None or session._secondary_model is None or session._regime_artifact is None
    if any_missing and cfg.train_ticks > 0:
        session.train_on_recent(cfg.train_ticks)
    still_missing = session._model is None or session._secondary_model is None or session._regime_artifact is None
    if not still_missing:
        session._validate_production_stack()
    else:
        missing = []
        if session._model is None:
            missing.append("primary alpha model")
        if session._secondary_model is None:
            missing.append("secondary alpha model")
        if session._regime_artifact is None:
            missing.append("regime model")
        if session._model is None and cfg.require_full_model_stack:
            raise RuntimeError(
                "Research production stack requires a trained model. Collect more ticks before starting, set --train-ticks, or use --no-require-full-model-stack for non-production debugging."
            )
    session.run()


if __name__ == "__main__":
    main()
