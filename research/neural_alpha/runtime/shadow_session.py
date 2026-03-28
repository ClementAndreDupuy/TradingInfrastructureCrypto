from __future__ import annotations
import json
import mmap
import os
import signal
import struct
import time
import traceback
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable
import numpy as np
import polars as pl
import torch
from torch.utils.data import DataLoader
from research.regime import (
    RegimeConfig,
    RegimeSignalPublisher,
    infer_regime_probabilities,
    load_regime_artifact,
    save_regime_artifact_bundle,
    train_regime_model_from_df,
)
from .core_bridge import CoreBridge, RING_PATH
from ..data.dataset import DatasetConfig, LOBDataset, rolling_normalise
from ..data.features import compute_lob_tensor, compute_scalar_features
from ..models.model import CryptoAlphaNet
from ..models.trainer import TrainerConfig, walk_forward_train
from ..operations.governance import ChampionChallengerRegistry, DriftGuard, EnsembleCanary
from ..pipeline import (
    _auto_tune_trainer_config,
    _DIRECTION_LOSS_THRESHOLD,
    _fetch_binance_l5,
    _fetch_coinbase_l5,
    _fetch_kraken_l5,
    _fetch_okx_l5,
    _selection_score,
    _validate_alpha_input_schema,
    collect_from_core_bridge,
)
from ..._config import model_cfg, shadow_cfg

_scfg = shadow_cfg()
_ipc = _scfg["ipc"]
_sess = _scfg["session"]
_mcfg = model_cfg()
_trainer_cfg = _mcfg["trainer"]
_secondary_cfg = _mcfg["secondary"]
_SECONDARY_D_SPATIAL: int = _secondary_cfg["d_spatial"]
_SECONDARY_D_TEMPORAL: int = _secondary_cfg["d_temporal"]
_SECONDARY_N_TEMP_LAYERS: int = _secondary_cfg["n_temp_layers"]


def _utcnow() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"

_SIGNAL_FILE: str = _ipc["signal_file"]
_SIGNAL_SIZE = 48
_SEQ_FMT = "=Q"
_SEQ_OFFSET = 0
_DATA_FMT = "=dddqq"
_DATA_OFFSET = 8
_REGIME_SIGNAL_FILE: str = _ipc["regime_signal_file"]
_MAX_EXCHANGE_JUMP_NS: int = _ipc["max_exchange_jump_ns"]
_DIRECTION_GATE: float = _scfg["direction_gate"]
_HORIZON_CANDIDATES: tuple = tuple(_scfg["horizon_candidates"])
_GATING_KEYS = ("confidence_gate", "horizon_disagreement_gate", "safe_mode_gate")
_NOTIONAL_SCALE_BUCKETS: tuple[tuple[float, float], ...] = (
    (0.90, 1.0),
    (0.75, 0.35),
    (0.60, 0.15),
    (0.0, 0.05),
)
FetchFn = Callable[[str], dict[str, Any] | None]
_FETCHERS = {"BINANCE": _fetch_binance_l5, "KRAKEN": _fetch_kraken_l5, "OKX": _fetch_okx_l5, "COINBASE": _fetch_coinbase_l5}
_TRADE_FLOW_SCHEMA_DEFAULTS: dict[str, Any] = {
    "trade_direction": 255,
}


def _finite_float(value: Any, default: float = 0.0) -> float:
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        return float(default)
    if not np.isfinite(parsed):
        return float(default)
    return parsed


def _sanitise_signal_info(signal_info: dict[str, Any]) -> dict[str, Any]:
    sanitised = dict(signal_info)
    float_fields = (
        "mid_price",
        "ret_1tick_bps",
        "ret_10tick_bps",
        "ret_mid_bps",
        "ret_long_bps",
        "risk_score",
        "size_fraction",
        "signal",
        "dir_p_down",
        "dir_p_flat",
        "dir_p_up",
        "primary_signal",
        "ensemble_signal",
        "p_calm",
        "p_trending",
        "p_shock",
        "p_illiquid",
    )
    for field in float_fields:
        sanitised[field] = _finite_float(sanitised.get(field, 0.0), 0.0)
    sanitised["horizon_ticks"] = int(sanitised.get("horizon_ticks", 0) or 0)
    sanitised["gating_reasons"] = list(sanitised.get("gating_reasons", []))
    return sanitised


def _ensure_trade_flow_schema(df: pl.DataFrame) -> pl.DataFrame:
    if len(df) == 0:
        return df
    out = df
    for column, default in _TRADE_FLOW_SCHEMA_DEFAULTS.items():
        if column not in out.columns:
            out = out.with_columns(pl.lit(default).alias(column))
    return out


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
    health_state: str = "starting"
    health_transition_count: int = 0
    health_transition_root_causes: dict[str, int] = field(default_factory=lambda: {
        "snapshot_rejection": 0,
        "resnapshot_loop": 0,
        "bridge_gap": 0,
        "recovered": 0,
    })
@dataclass
class ShadowSessionConfig:
    model_path: str | None = None
    secondary_model_path: str | None = None
    log_path: str = _sess["log_path"]
    interval_ms: int = _sess["interval_ms"]
    duration_s: int = _sess["duration_s"]
    report_interval_s: int = _sess["report_interval_s"]
    seq_len: int = _sess["seq_len"]
    d_spatial: int = _sess["d_spatial"]
    d_temporal: int = _sess["d_temporal"]
    train_ticks: int = _sess["train_ticks"]
    train_epochs: int = _sess["train_epochs"]
    symbol: str = _sess["symbol"]
    exchanges: list[str] = field(default_factory=lambda: list(_sess["exchanges"]))
    signal_file: str = _SIGNAL_FILE
    regime_signal_file: str = _REGIME_SIGNAL_FILE
    lob_feed_path: str = RING_PATH
    regime_model_path: str = _sess["regime_model_path"]
    registry_path: str = _sess["registry_path"]
    drift_window: int = _sess["drift_window"]
    drift_min_samples: int = _sess["drift_min_samples"]
    drift_ic_floor: float = _sess["drift_ic_floor"]
    safe_mode_ticks: int = _sess["safe_mode_ticks"]
    continuous_train_every_ticks: int = _sess["continuous_train_every_ticks"]
    continuous_train_window_ticks: int = _sess["continuous_train_window_ticks"]
    regime_retrain_every_ticks: int = _sess["regime_retrain_every_ticks"]
    canary_ic_margin: float = _sess["canary_ic_margin"]
    canary_icir_floor: float = _sess["canary_icir_floor"]
    canary_window: int = _sess["canary_window"]
    canary_min_samples: int = _sess["canary_min_samples"]
    canary_trigger_streak: int = _sess.get("canary_trigger_streak", 3)
    canary_rearm_streak: int = _sess.get("canary_rearm_streak", 8)
    safe_mode_cooldown_ticks: int = _sess.get("safe_mode_cooldown_ticks", 120)
    drift_breach_streak: int = _sess.get("drift_breach_streak", 3)
    drift_recovery_streak: int = _sess.get("drift_recovery_streak", 8)
    drift_rearm_ic_margin: float = _sess.get("drift_rearm_ic_margin", 0.02)
    extreme_risk_threshold: float = _sess.get("extreme_risk_threshold", 0.98)
    secondary_max_relative_loss: float = _sess.get("secondary_max_relative_loss", 1.15)
    secondary_min_validation_ic_delta: float = _sess.get("secondary_min_validation_ic_delta", -0.02)
    require_full_model_stack: bool = _sess["require_full_model_stack"]
    min_continuous_train_interval_s: int = _sess["min_continuous_train_interval_s"]
    min_regime_train_interval_s: int = _sess["min_regime_train_interval_s"]
    regime_startup_warmup_s: int = _sess["regime_startup_warmup_s"]
    max_regime_switches_per_minute_for_retrain: float = _sess.get("max_regime_switches_per_minute_for_retrain", 6.0)
    retrain_hpo_score_explosion_factor: float = _sess.get("retrain_hpo_score_explosion_factor", 8.0)
    primary_w_return: float = _trainer_cfg["w_return"]
    primary_w_direction: float = _trainer_cfg["w_direction"]
    primary_w_risk: float = _trainer_cfg["w_risk"]
    secondary_w_return: float = _secondary_cfg["w_return"]
    secondary_w_direction: float = _secondary_cfg["w_direction"]
    secondary_w_risk: float = _secondary_cfg["w_risk"]
class _SignalPublisher:
    def __init__(self, path: str = _SIGNAL_FILE) -> None:
        self._path = path
        _ensure_parent_dir(path)
        self._file = open(path, "w+b")
        self._file.write(b"\x00" * _SIGNAL_SIZE)
        self._file.flush()
        self._mapping = mmap.mmap(self._file.fileno(), _SIGNAL_SIZE)
    def publish(self, signal_bps: float, risk_score: float, size_fraction: float, horizon_ticks: int) -> None:
        ts_ns = time.time_ns()
        seq: int = struct.unpack_from(_SEQ_FMT, self._mapping, _SEQ_OFFSET)[0]
        struct.pack_into(_SEQ_FMT, self._mapping, _SEQ_OFFSET, seq + 1)
        struct.pack_into(_DATA_FMT, self._mapping, _DATA_OFFSET, signal_bps, risk_score, float(size_fraction), int(horizon_ticks), ts_ns)
        struct.pack_into(_SEQ_FMT, self._mapping, _SEQ_OFFSET, seq + 2)
        self._mapping.flush()
    def close(self) -> None:
        try:
            self._mapping.close()
            self._file.close()
            os.unlink(self._path)
        except OSError:
            pass
def _ensure_parent_dir(path: str | Path) -> None:
    Path(path).expanduser().resolve().parent.mkdir(parents=True, exist_ok=True)
def _symbol_model_path(symbol: str, variant: str = "latest") -> Path:
    return Path(_scfg["model_path_template"].format(symbol=symbol.lower(), variant=variant))
def _extract_tick_timestamps(tick: dict[str, Any]) -> tuple[int, int]:
    exchange_ts = int(tick.get("timestamp_exchange_ns", tick.get("exchange_timestamp_ns", tick.get("timestamp_ns", 0))))
    local_ts = int(tick.get("timestamp_local_ns", tick.get("local_timestamp_ns", tick.get("timestamp_ns", 0))))
    return exchange_ts, local_ts
def _ordered_records(records: list[dict[str, Any]]) -> list[dict[str, Any]]:
    return sorted(records, key=lambda record: (int(record.get("event_index", 0)), int(record.get("session_elapsed_ns", 0))))
def _build_signal_alignment(records: list[dict[str, Any]]) -> tuple[np.ndarray, np.ndarray]:
    signals: list[float] = []
    outcomes: list[float] = []
    ordered = _ordered_records(records)
    for index in range(len(ordered) - 1):
        current = ordered[index]
        following = ordered[index + 1]
        current_mid = float(current.get("mid_price", 0.0))
        next_mid = float(following.get("mid_price", 0.0))
        if current_mid <= 0.0 or next_mid <= 0.0:
            continue
        signals.append(float(current.get("signal", 0.0)))
        outcomes.append((next_mid - current_mid) / current_mid)
    return np.asarray(signals, dtype=np.float64), np.asarray(outcomes, dtype=np.float64)
def _extract_signal_series(records: list[dict[str, Any]]) -> tuple[np.ndarray, np.ndarray]:
    ordered = _ordered_records(records)
    raw_signals_bps = np.asarray([float(record.get("signal", 0.0)) * 10_000.0 for record in ordered], dtype=np.float64)
    effective_signals_bps = np.asarray([float(record.get("ret_mid_bps", 0.0)) for record in ordered], dtype=np.float64)
    return raw_signals_bps, effective_signals_bps

def _summarise_regime_churn(records: list[dict[str, Any]]) -> dict[str, float | int | str | None]:
    ordered = _ordered_records(records)
    if not ordered:
        return {
            "dominant_regime": None,
            "dominant_regime_change_count": 0,
            "switches_per_minute": 0.0,
            "average_dominant_confidence": 0.0,
        }
    regime_keys = ("p_calm", "p_trending", "p_shock", "p_illiquid")
    previous_regime: str | None = None
    change_count = 0
    confidence_sum = 0.0
    confidence_count = 0
    last_regime: str | None = None
    for record in ordered:
        probs = {key: float(record.get(key, 0.0)) for key in regime_keys}
        dominant_key = max(probs, key=probs.get)
        dominant_regime = dominant_key[2:]
        dominant_confidence = probs[dominant_key]
        if previous_regime is not None and dominant_regime != previous_regime:
            change_count += 1
        previous_regime = dominant_regime
        last_regime = dominant_regime
        confidence_sum += dominant_confidence
        confidence_count += 1
    start_elapsed = int(ordered[0].get("session_elapsed_ns", 0))
    end_elapsed = int(ordered[-1].get("session_elapsed_ns", 0))
    elapsed_min = max((end_elapsed - start_elapsed) / 60_000_000_000.0, 1e-09)
    return {
        "dominant_regime": last_regime,
        "dominant_regime_change_count": change_count,
        "switches_per_minute": float(change_count / elapsed_min),
        "average_dominant_confidence": float(confidence_sum / max(confidence_count, 1)),
    }
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
    prev_local: int | None = None
    prev_event_index: int | None = None
    prev_venue_exchange_ts: dict[str, int] = {}
    for record in _ordered_records(records):
        exchange_ts = int(record.get("timestamp_exchange_ns", 0))
        local_ts = int(record.get("timestamp_local_ns", 0))
        event_index = int(record.get("event_index", 0))
        venue = str(record.get("exchange", "UNKNOWN"))
        if exchange_ts <= 0:
            diagnostics["exchange_missing"] = int(diagnostics["exchange_missing"]) + 1
        if local_ts <= 0:
            diagnostics["local_missing"] = int(diagnostics["local_missing"]) + 1
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
            if exchange_ts < previous_venue_ts:
                diagnostics["exchange_non_monotonic"] = int(diagnostics["exchange_non_monotonic"]) + 1
                diagnostics["exchange_monotonic"] = False
            elif jump > _MAX_EXCHANGE_JUMP_NS:
                diagnostics["cross_venue_exchange_jumps"] = int(diagnostics["cross_venue_exchange_jumps"]) + 1
        if exchange_ts > 0:
            prev_venue_exchange_ts[venue] = exchange_ts
        if local_ts > 0:
            prev_local = local_ts
        prev_event_index = event_index
    diagnostics["has_timestamp_issues"] = any(int(diagnostics[key]) > 0 for key in (
        "exchange_missing",
        "local_missing",
        "exchange_non_monotonic",
        "local_non_monotonic",
        "event_index_non_monotonic",
        "cross_venue_exchange_jumps",
    ))
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
        self._regime_artifact = self._load_regime_artifact(cfg.regime_model_path)
        self._running = False
        self._signal_records: list[dict[str, Any]] = []
        self._registry = ChampionChallengerRegistry(cfg.registry_path)
        self._drift_guard = DriftGuard(
            window=cfg.drift_window,
            min_samples=cfg.drift_min_samples,
            ic_floor=cfg.drift_ic_floor,
            breach_streak_required=cfg.drift_breach_streak,
            recovery_streak_required=cfg.drift_recovery_streak,
            rearm_ic_margin=cfg.drift_rearm_ic_margin,
        )
        self._safe_mode_ticks_remaining = 0
        self._safe_mode_cooldown_ticks_remaining = 0
        self._safe_mode_reason: str | None = None
        self._bridge = CoreBridge(cfg.lob_feed_path)
        self._bridge.open()
        self._processed_ticks = 0
        self._last_continuous_train_tick = 0
        self._last_regime_train_tick = 0
        self._session_event_index = 0
        self._session_wall_start_ns = time.time_ns()
        self._session_steady_start_ns = time.monotonic_ns()
        self._last_continuous_train_steady_ns = self._session_steady_start_ns
        self._last_regime_train_steady_ns = self._session_steady_start_ns
        self._canary: EnsembleCanary | None = None
        self._prev_primary_signal: float | None = None
        self._prev_ensemble_signal: float | None = None
        self._prev_regime_probs: dict[str, float] | None = None
        self._venue_stats = {exchange.upper(): VenueRuntimeStats() for exchange in cfg.exchanges}
        self._gating_reason_counts = {key: 0 for key in _GATING_KEYS}
        self._retrain_failure_counts: dict[str, int] = {}
        self._regime_retrain_failure_counts: dict[str, int] = {}
        self._latest_primary_sanity: dict[str, float] = {"validation_loss": float("inf"), "validation_ic": 0.0}
        self._latest_secondary_sanity: dict[str, float] = {}
    def _continuous_retrain_guard(self) -> tuple[bool, str]:
        diagnostics = _summarise_timestamp_quality(self._signal_records)
        _corruption_keys = (
            "exchange_missing",
            "local_missing",
            "exchange_non_monotonic",
            "local_non_monotonic",
            "event_index_non_monotonic",
        )
        if any(int(diagnostics[key]) > 0 for key in _corruption_keys):
            return False, "ts_quality_warn"
        churn = _summarise_regime_churn(self._signal_records)
        switches_per_minute = float(churn["switches_per_minute"])
        if switches_per_minute > self.cfg.max_regime_switches_per_minute_for_retrain:
            return False, (
                f"regime_switch_rate_high switches_per_minute={switches_per_minute:.4f} "
                f"cap={self.cfg.max_regime_switches_per_minute_for_retrain:.4f}"
            )
        return True, "ok"
    @staticmethod
    def _load_regime_artifact(path: str) -> Any:
        if not Path(path).exists():
            return None
        try:
            return load_regime_artifact(path)
        except Exception:
            return None
    def _build_model(self, d_spatial: int = 64, d_temporal: int = 128, n_temp_layers: int = 3) -> CryptoAlphaNet:
        return CryptoAlphaNet(d_spatial=d_spatial, d_temporal=d_temporal, n_temp_layers=n_temp_layers, seq_len=self.cfg.seq_len).to(self._device).eval()
    def load_model(self, path: str) -> None:
        self._model = self._build_model(self.cfg.d_spatial, self.cfg.d_temporal)
        self._model.load_state_dict(torch.load(path, map_location=self._device, weights_only=True))
    def load_secondary_model(self, path: str) -> None:
        meta_path = Path(path).with_suffix(".json")
        if meta_path.exists():
            try:
                meta = json.loads(meta_path.read_text(encoding="utf-8"))
                if meta.get("sanity_passed") is False:
                    self._secondary_model = None
                    self._canary = None
                    return
            except Exception:
                pass
        self._secondary_model = self._build_model(
            d_spatial=_SECONDARY_D_SPATIAL,
            d_temporal=_SECONDARY_D_TEMPORAL,
            n_temp_layers=_SECONDARY_N_TEMP_LAYERS,
        )
        self._secondary_model.load_state_dict(torch.load(path, map_location=self._device, weights_only=True))
        self._reset_canary()
    def _reset_canary(self) -> None:
        self._canary = EnsembleCanary(
            window=self.cfg.canary_window,
            min_samples=self.cfg.canary_min_samples,
            ic_margin=self.cfg.canary_ic_margin,
            icir_floor=self.cfg.canary_icir_floor,
            trigger_streak_required=self.cfg.canary_trigger_streak,
            rearm_streak_required=self.cfg.canary_rearm_streak,
        )
        self._prev_primary_signal = None
        self._prev_ensemble_signal = None
    def _missing_model_stack(self) -> list[str]:
        missing: list[str] = []
        if self._model is None:
            missing.append("primary alpha model")
        if self._secondary_model is None:
            missing.append("secondary alpha model")
        if self._regime_artifact is None:
            missing.append("regime model")
        return missing
    def _validate_production_stack(self) -> None:
        missing = self._missing_model_stack()
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
        rows: list[dict[str, Any]] = []
        interval_s = self.cfg.interval_ms / 1000.0
        while len(rows) < n_ticks:
            rows.extend(self._fetch_tick())
            if len(rows) < n_ticks:
                time.sleep(interval_s)
        if not rows:
            raise RuntimeError("No data collected for training — core bridge and collectors unavailable.")
        return pl.DataFrame(rows[:n_ticks]).sort("timestamp_ns")
    def _training_folds(self, df: pl.DataFrame) -> int:
        return min(2, max(1, len(df) // (4 * self.cfg.seq_len)))
    def _primary_trainer_config(self, resume_state: dict[str, torch.Tensor] | None, n_folds: int) -> TrainerConfig:
        return TrainerConfig(
            epochs=self.cfg.train_epochs,
            n_folds=n_folds,
            seq_len=self.cfg.seq_len,
            d_spatial=self.cfg.d_spatial,
            d_temporal=self.cfg.d_temporal,
            resume_state_dict=resume_state,
            lr_warmup_epochs=min(3, self.cfg.train_epochs // 4),
            early_stop_patience=4,
            log_every_epochs=1,
            verbose=False,
            w_return=self.cfg.primary_w_return,
            w_direction=self.cfg.primary_w_direction,
            w_risk=self.cfg.primary_w_risk,
        )
    def _secondary_trainer_config(self, n_folds: int) -> TrainerConfig:
        return TrainerConfig(
            epochs=self.cfg.train_epochs,
            n_folds=n_folds,
            seq_len=self.cfg.seq_len,
            d_spatial=_SECONDARY_D_SPATIAL,
            d_temporal=_SECONDARY_D_TEMPORAL,
            n_temp_layers=_SECONDARY_N_TEMP_LAYERS,
            fold_seed_offset=9999,
            lr_warmup_epochs=min(3, self.cfg.train_epochs // 4),
            early_stop_patience=4,
            log_every_epochs=1,
            verbose=False,
            w_return=self.cfg.secondary_w_return,
            w_direction=self.cfg.secondary_w_direction,
            w_risk=self.cfg.secondary_w_risk,
        )
    def _snapshot_current_state(self) -> dict[str, torch.Tensor] | None:
        return None if self._model is None else {key: value.detach().cpu().clone() for key, value in self._model.state_dict().items()}
    def _save_state_artifacts(self, state_dict: dict[str, torch.Tensor], out_path: Path, meta: dict[str, Any]) -> None:
        out_path.parent.mkdir(parents=True, exist_ok=True)
        tmp_out = out_path.with_suffix(out_path.suffix + ".tmp")
        torch.save(state_dict, tmp_out)
        tmp_out.replace(out_path)
        meta_path = out_path.with_suffix(".json")
        tmp_meta = meta_path.with_suffix(meta_path.suffix + ".tmp")
        tmp_meta.write_text(json.dumps(meta, indent=2), encoding="utf-8")
        tmp_meta.replace(meta_path)
    @staticmethod
    def _best_effort(action: Callable[[], None]) -> None:
        try:
            action()
        except Exception:
            pass
    def _evaluate_state_on_holdout(self, state_dict: dict[str, torch.Tensor], holdout_df: pl.DataFrame) -> tuple[float, float]:
        dataset = LOBDataset(holdout_df, DatasetConfig(seq_len=self.cfg.seq_len))
        if len(dataset) == 0:
            return float("inf"), float("inf")
        loader = DataLoader(dataset, batch_size=64, shuffle=False)
        model = self._build_model(self.cfg.d_spatial, self.cfg.d_temporal)
        try:
            model.load_state_dict(state_dict, strict=True)
        except RuntimeError:
            return float("inf"), float("inf")
        sqerr = 0.0
        count = 0
        dir_loss_sum = 0.0
        dir_batches = 0
        with torch.no_grad():
            for batch in loader:
                outputs = model(batch["lob"].to(self._device), batch["scalar"].to(self._device))
                pred_mid = outputs["returns"][:, -1, 2]
                true_mid = batch["labels"][:, -1, 2].to(self._device)
                diff = pred_mid - true_mid
                sqerr += float((diff * diff).sum().item())
                count += int(diff.numel())
                dir_logits = outputs["direction"][:, -1, :]
                dir_targets = batch["labels"][:, -1, 4].long().to(self._device)
                dir_loss_sum += float(torch.nn.functional.cross_entropy(dir_logits, dir_targets).item())
                dir_batches += 1
        return sqerr / max(count, 1), dir_loss_sum / max(dir_batches, 1)
    def _select_primary_state(
        self,
        df: pl.DataFrame,
        candidate_state: dict[str, torch.Tensor],
        resume_state: dict[str, torch.Tensor] | None,
    ) -> tuple[dict[str, torch.Tensor], dict[str, Any]]:
        summary: dict[str, Any] = {"incumbent": None, "challenger": None, "selected": "challenger"}
        holdout_df = df[int(len(df) * 0.8) :]
        if len(holdout_df) < self.cfg.seq_len * 2:
            return candidate_state, summary
        challenger_mse, challenger_dir_loss = self._evaluate_state_on_holdout(candidate_state, holdout_df)
        summary["challenger"] = challenger_mse
        summary["challenger_dir_loss"] = challenger_dir_loss
        if challenger_dir_loss >= _DIRECTION_LOSS_THRESHOLD:
            print(
                f"[{_utcnow()}] [Shadow] challenger rejected — direction loss {challenger_dir_loss:.4f} >= {_DIRECTION_LOSS_THRESHOLD}"
            )
            summary["selected"] = "rejected_anti_predictive"
            if resume_state is None:
                return candidate_state, summary
            incumbent_mse, _ = self._evaluate_state_on_holdout(resume_state, holdout_df)
            summary["incumbent"] = incumbent_mse
            summary["selected"] = "incumbent"
            return resume_state, summary
        if resume_state is None:
            return candidate_state, summary
        incumbent_mse, _ = self._evaluate_state_on_holdout(resume_state, holdout_df)
        summary["incumbent"] = incumbent_mse
        if challenger_mse <= incumbent_mse:
            return candidate_state, summary
        summary["selected"] = "incumbent"
        return resume_state, summary
    def _train_secondary_on_data(self, df: pl.DataFrame, out_path: Path) -> None:
        cfg = _auto_tune_trainer_config(
            df,
            self._secondary_trainer_config(self._training_folds(df)),
            secondary=True,
            enabled=True,
        )
        fold_results = walk_forward_train(
            df,
            cfg,
        )
        if not fold_results:
            return
        best = min(fold_results, key=_selection_score)
        state = best["model_state"]
        metrics = dict(best.get("metrics", {}))
        primary_loss = float(self._latest_primary_sanity.get("validation_loss", float("inf")))
        primary_ic = float(self._latest_primary_sanity.get("validation_ic", 0.0))
        secondary_loss = float(best.get("best_selection_score", float("inf")))
        previous_secondary_loss = float(self._latest_secondary_sanity.get("validation_loss", float("inf")))
        if np.isfinite(previous_secondary_loss):
            max_allowed_secondary_loss = previous_secondary_loss * self.cfg.retrain_hpo_score_explosion_factor
            if secondary_loss > max_allowed_secondary_loss:
                print(
                    f"[{_utcnow()}] [Shadow] secondary retrain aborted — score explosion"
                    f" secondary_loss={secondary_loss:.6f}"
                    f" previous_secondary_loss={previous_secondary_loss:.6f}"
                    f" max_allowed_secondary_loss={max_allowed_secondary_loss:.6f}"
                )
                return
        secondary_ic = self._estimate_validation_ic(fold_results)
        self._latest_secondary_sanity = {
            "validation_loss": secondary_loss,
            "validation_ic": secondary_ic,
            "primary_validation_loss": primary_loss,
            "primary_validation_ic": primary_ic,
        }
        weak_secondary = (
            secondary_loss > primary_loss * self.cfg.secondary_max_relative_loss
            or secondary_ic < primary_ic + self.cfg.secondary_min_validation_ic_delta
        )
        if weak_secondary:
            print(
                f"[{_utcnow()}] [Shadow] secondary model disabled — failed sanity checks"
                f" secondary_loss={secondary_loss:.6f}"
                f" primary_loss={primary_loss:.6f}"
                f" secondary_ic={secondary_ic:.6f}"
                f" primary_ic={primary_ic:.6f}"
            )
            out_path.parent.mkdir(parents=True, exist_ok=True)
            if out_path.exists():
                out_path.unlink()
            out_path.with_suffix(".json").write_text(
                json.dumps(
                    {
                        "trained_at_ns": time.time_ns(),
                        "sanity_passed": False,
                        "validation_loss": secondary_loss,
                        "validation_ic": secondary_ic,
                        "primary_validation_loss": primary_loss,
                        "primary_validation_ic": primary_ic,
                    },
                    indent=2,
                ),
                encoding="utf-8",
            )
            self._secondary_model = None
            self._canary = None
            return
        self._save_state_artifacts(
            state,
            out_path,
            {
                "trained_at_ns": time.time_ns(),
                "train_ticks": len(df),
                "train_epochs": self.cfg.train_epochs,
                "seq_len": self.cfg.seq_len,
                "d_spatial": cfg.d_spatial,
                "d_temporal": cfg.d_temporal,
                "n_temp_layers": cfg.n_temp_layers,
                "metrics": metrics,
                "validation_loss": secondary_loss,
                "validation_ic": secondary_ic,
                "sanity_passed": True,
            },
        )
        self._secondary_model = self._build_model(
            d_spatial=cfg.d_spatial,
            d_temporal=cfg.d_temporal,
            n_temp_layers=cfg.n_temp_layers,
        )
        self._secondary_model.load_state_dict(state)
        self._reset_canary()
    @staticmethod
    def _estimate_validation_ic(fold_results: list[dict[str, Any]]) -> float:
        values: list[float] = []
        for fold in fold_results:
            preds = fold.get("predictions")
            labels = fold.get("labels")
            if preds is None or labels is None:
                continue
            pred_mid = np.asarray(preds)[:, 2]
            label_mid = np.asarray(labels)[:, 2]
            if pred_mid.size < 2 or label_mid.size < 2:
                continue
            if float(np.std(pred_mid)) <= 1e-9 or float(np.std(label_mid)) <= 1e-9:
                continue
            values.append(float(np.corrcoef(pred_mid, label_mid)[0, 1]))
        return float(np.mean(values)) if values else 0.0
    def _train_regime_on_data(self, df: pl.DataFrame) -> None:
        regime_path = Path(self.cfg.regime_model_path)
        artifact, _ = train_regime_model_from_df(df, RegimeConfig())
        spread_means = [float(m[1]) for m in artifact.means]
        spread_range = max(spread_means) - min(spread_means)
        if spread_range < 0.3:
            print(
                f"[{_utcnow()}] [Shadow] regime retrain skipped — degenerate clusters"
                f"  spread_range={spread_range:.4f} (< 0.3 threshold)"
            )
            return
        regime_path.parent.mkdir(parents=True, exist_ok=True)
        save_regime_artifact_bundle(
            artifact,
            str(regime_path),
            metadata={
                "trained_at_ns": time.time_ns(),
                "train_rows": len(df),
                "train_window_ticks": max(self.cfg.seq_len * 4, self.cfg.continuous_train_window_ticks),
                "regime_retrain_every_ticks": self.cfg.regime_retrain_every_ticks,
                "spread_means": spread_means,
                "spread_range": spread_range,
                "regime_names": artifact.regime_names,
                "artifact_version": artifact.version,
            },
        )
        self._regime_artifact = artifact
        self._prev_regime_probs = None
        names = ", ".join(artifact.regime_names)
        print(f"[{_utcnow()}] [Shadow] regime retrain done  regimes=[{names}]")
    def train_on_recent(self, n_ticks: int) -> None:
        df = self._collect_training_ticks(n_ticks)
        _validate_alpha_input_schema(df)
        resume_state = self._snapshot_current_state()
        previous_primary_loss = float(self._latest_primary_sanity.get("validation_loss", float("inf")))
        primary_cfg = _auto_tune_trainer_config(
            df,
            self._primary_trainer_config(resume_state, self._training_folds(df)),
            secondary=False,
            enabled=True,
        )
        fold_results = walk_forward_train(df, primary_cfg)
        if not fold_results:
            return
        best = min(fold_results, key=_selection_score)
        best_primary_loss = float(best.get("best_selection_score", float("inf")))
        if np.isfinite(previous_primary_loss):
            max_allowed_primary_loss = previous_primary_loss * self.cfg.retrain_hpo_score_explosion_factor
            if best_primary_loss > max_allowed_primary_loss:
                print(
                    f"[{_utcnow()}] [Shadow] primary retrain aborted — score explosion"
                    f" best_selection_score={best_primary_loss:.6f}"
                    f" previous_best_selection_score={previous_primary_loss:.6f}"
                    f" max_allowed_selection_score={max_allowed_primary_loss:.6f}"
                )
                return
        self._latest_primary_sanity = {
            "validation_loss": best_primary_loss,
            "validation_ic": self._estimate_validation_ic(fold_results),
        }
        candidate_state = best["model_state"]
        metrics = dict(best.get("metrics", {}))
        selected_state, holdout_summary = self._select_primary_state(df, candidate_state, resume_state)
        self._model = self._build_model(self.cfg.d_spatial, self.cfg.d_temporal)
        self._model.load_state_dict(selected_state)
        loss = metrics.get("loss_total", float("nan"))
        _raw_oos = holdout_summary.get("challenger") if isinstance(holdout_summary, dict) else None
        oos_mse = None if _raw_oos is None else float(_raw_oos)
        selected = holdout_summary.get("selected", "challenger") if isinstance(holdout_summary, dict) else "challenger"
        oos_mse_text = "n/a" if oos_mse is None or not np.isfinite(oos_mse) else f"{oos_mse:.6f}"
        print(
            f"[{_utcnow()}] [Shadow] model retrain done"
            f"  ticks={n_ticks}"
            f"  best_fold_loss={loss:.6f}"
            f"  oos_mse={oos_mse_text}"
            f"  selected={selected}"
        )
        out_path = Path(self.cfg.model_path) if self.cfg.model_path else _symbol_model_path(self.cfg.symbol)
        self._save_state_artifacts(
            selected_state,
            out_path,
            {
                "trained_at_ns": time.time_ns(),
                "train_ticks": n_ticks,
                "train_epochs": self.cfg.train_epochs,
                "seq_len": self.cfg.seq_len,
                "d_spatial": self.cfg.d_spatial,
                "d_temporal": self.cfg.d_temporal,
                "metrics": metrics,
                "oos_holdout_mse": holdout_summary,
            },
        )
        secondary_path = Path(self.cfg.secondary_model_path) if self.cfg.secondary_model_path else _symbol_model_path(self.cfg.symbol, "secondary")
        self._best_effort(lambda: self._train_secondary_on_data(df, secondary_path))
    def _build_inference_inputs(self) -> tuple[torch.Tensor, torch.Tensor] | None:
        if self._model is None or len(self._ring) < self.cfg.seq_len:
            return None
        df_ring = _ensure_trade_flow_schema(pl.DataFrame(self._ring))
        lob_t = torch.from_numpy(compute_lob_tensor(df_ring)[-self.cfg.seq_len :]).unsqueeze(0).to(self._device)
        scalar_t = torch.from_numpy(rolling_normalise(compute_scalar_features(df_ring))[-self.cfg.seq_len :]).unsqueeze(0).to(self._device)
        return lob_t, scalar_t
    def _confidence_scale(self, confidence: float) -> float:
        for threshold, scale in _NOTIONAL_SCALE_BUCKETS:
            if confidence >= threshold:
                return scale
        return _NOTIONAL_SCALE_BUCKETS[-1][1]
    def _apply_signal_gates(
        self, raw_signal_bps: float, risk: float, dir_probs: np.ndarray, ret_1tick_bps: float
    ) -> tuple[float, float, list[str], bool]:
        signal_bps = raw_signal_bps
        size_scale = 1.0
        gating_reasons: list[str] = []
        if signal_bps > 0 and dir_probs[2] <= _DIRECTION_GATE or signal_bps < 0 and dir_probs[0] <= _DIRECTION_GATE:
            direction_confidence = float(dir_probs[2] if signal_bps > 0 else dir_probs[0])
            size_scale *= self._confidence_scale(direction_confidence)
            gating_reasons.append("confidence_gate")
        if signal_bps > 0 and ret_1tick_bps < 0 or signal_bps < 0 and ret_1tick_bps > 0:
            size_scale *= 0.35
            gating_reasons.append("horizon_disagreement_gate")
        safe_mode_active = self._safe_mode_ticks_remaining > 0
        if safe_mode_active:
            signal_bps = 0.0
            size_scale = 0.0
            self._safe_mode_ticks_remaining -= 1
            gating_reasons.append("safe_mode_gate")
        if self._safe_mode_cooldown_ticks_remaining > 0:
            self._safe_mode_cooldown_ticks_remaining -= 1
        if risk >= self.cfg.extreme_risk_threshold:
            signal_bps = 0.0
            size_scale = 0.0
        for reason in set(gating_reasons):
            self._gating_reason_counts[reason] += 1
        return signal_bps, size_scale, gating_reasons, safe_mode_active
    def _infer_regime_probabilities(self) -> dict[str, float]:
        regime_probs = {"p_calm": 1.0, "p_trending": 0.0, "p_shock": 0.0, "p_illiquid": 0.0}
        if self._regime_artifact is not None:
            try:
                regime_probs = infer_regime_probabilities(
                    _ensure_trade_flow_schema(pl.DataFrame(self._ring)),
                    self._regime_artifact,
                    prev_probs=self._prev_regime_probs,
                )
                self._prev_regime_probs = regime_probs
            except Exception:
                pass
        self._regime_publisher.publish(regime_probs["p_calm"], regime_probs["p_trending"], regime_probs["p_shock"], regime_probs["p_illiquid"])
        return regime_probs
    def _infer(self) -> dict[str, Any] | None:
        inputs = self._build_inference_inputs()
        if inputs is None or self._model is None:
            return None
        lob_t, scalar_t = inputs
        with torch.no_grad():
            primary_output = self._model(lob_t, scalar_t)
        returns = np.nan_to_num(
            primary_output["returns"][0, -1].cpu().numpy(),
            nan=0.0,
            posinf=0.0,
            neginf=0.0,
        )
        risk = _finite_float(primary_output["risk"][0, -1].cpu().item(), 1.0)
        primary_signal = _finite_float(returns[2], 0.0)
        raw_signal_bps = primary_signal * 10_000.0
        if self._secondary_model is not None:
            with torch.no_grad():
                secondary_returns = np.nan_to_num(
                    self._secondary_model(lob_t, scalar_t)["returns"][0, -1].cpu().numpy(),
                    nan=0.0,
                    posinf=0.0,
                    neginf=0.0,
                )
            raw_signal_bps = (raw_signal_bps + _finite_float(secondary_returns[2], 0.0) * 10_000.0) / 2.0
        dir_probs = np.nan_to_num(
            torch.softmax(primary_output["direction"][0, -1], dim=-1).cpu().numpy(),
            nan=0.0,
            posinf=0.0,
            neginf=0.0,
        )
        ret_1tick_bps = _finite_float(returns[0], 0.0) * 10_000.0
        signal_bps, size_scale, gating_reasons, safe_mode_active = self._apply_signal_gates(
            raw_signal_bps, risk, dir_probs, ret_1tick_bps
        )
        selected_horizon_ticks = int(_HORIZON_CANDIDATES[int(np.argmax(np.abs(returns)))])
        size_fraction = 0.0 if signal_bps == 0.0 else float(np.clip(1.0 - risk, 0.0, 1.0) * size_scale)
        self._publisher.publish(signal_bps, risk, size_fraction, selected_horizon_ticks)
        last_tick = self._ring[-1]
        exchange_ts, local_ts = _extract_tick_timestamps(last_tick)
        self._session_event_index += 1
        mid_price = (
            _finite_float(last_tick.get("best_bid", last_tick.get("bid_price_1", 0.0)), 0.0)
            + _finite_float(last_tick.get("best_ask", last_tick.get("ask_price_1", 0.0)), 0.0)
        ) / 2.0
        signal_info = {
            "timestamp_ns": local_ts,
            "timestamp_exchange_ns": exchange_ts,
            "timestamp_local_ns": local_ts,
            "event_index": self._session_event_index,
            "session_wall_start_ns": self._session_wall_start_ns,
            "session_elapsed_ns": time.monotonic_ns() - self._session_steady_start_ns,
            "exchange": last_tick.get("exchange", "BINANCE"),
            "mid_price": mid_price,
            "ret_1tick_bps": ret_1tick_bps,
            "ret_10tick_bps": float(returns[1]) * 10_000.0,
            "ret_mid_bps": signal_bps,
            "ret_long_bps": float(returns[3]) * 10_000.0,
            "risk_score": risk,
            "size_fraction": size_fraction,
            "horizon_ticks": selected_horizon_ticks,
            "signal": float(returns[2]),
            "dir_p_down": float(dir_probs[0]),
            "dir_p_flat": float(dir_probs[1]),
            "dir_p_up": float(dir_probs[2]),
            "gated": signal_bps == 0.0 and raw_signal_bps != 0.0,
            "safe_mode": safe_mode_active,
            "safe_mode_reason": self._safe_mode_reason if safe_mode_active else None,
            "gating_reasons": gating_reasons,
            "primary_signal": primary_signal,
            "ensemble_signal": raw_signal_bps / 10_000.0,
            **self._infer_regime_probabilities(),
        }
        return _sanitise_signal_info(signal_info)
    def _trigger_safe_mode(self, reason: str) -> None:
        if self._safe_mode_cooldown_ticks_remaining > 0:
            return
        self._safe_mode_ticks_remaining = max(self._safe_mode_ticks_remaining, self.cfg.safe_mode_ticks)
        self._safe_mode_cooldown_ticks_remaining = self.cfg.safe_mode_cooldown_ticks
        self._safe_mode_reason = reason
        rollback_path = self._registry.rollback_to_previous_champion(reason=reason)
        if rollback_path and Path(rollback_path).exists():
            self._best_effort(lambda: self.load_model(rollback_path))
    def _transition_venue_health(self, exchange: str, new_state: str, root_cause: str) -> None:
        stats = self._venue_stats[exchange]
        if stats.health_state == new_state:
            return
        stats.health_state = new_state
        stats.health_transition_count += 1
        stats.health_transition_root_causes[root_cause] = stats.health_transition_root_causes.get(root_cause, 0) + 1

    def _rest_fallback_tick(self, exchange: str, fetcher: FetchFn | None) -> dict[str, Any] | None:
        if fetcher is None:
            return None
        row = fetcher(self.cfg.symbol)
        if row is None:
            return None
        stats = self._venue_stats[exchange]
        stats.rest_fallback_usage += 1
        stats.last_source = "rest_fallback"
        return {**row, "exchange": exchange, "tick_source": "rest_fallback"}
    def _fetch_tick(self) -> list[dict[str, Any]]:
        bridge_ticks = self._bridge.read_new_ticks()
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
                self._transition_venue_health(exchange, "healthy", "recovered")
                continue
            stats.missing_venue_incidents += 1
            stats.consecutive_missing_polls += 1
            stats.last_source = "bridge_unavailable" if not stats.startup_confirmed else "bridge_resnapshot"
            if stats.startup_confirmed:
                if stats.consecutive_missing_polls >= 10:
                    stats.resnapshot_count += 1
                    self._transition_venue_health(exchange, "degraded", "resnapshot_loop")
            else:
                self._transition_venue_health(exchange, "degraded", "snapshot_rejection")
            fallback_tick = self._rest_fallback_tick(exchange, _FETCHERS.get(exchange))
            if fallback_tick is not None:
                rest_ticks.append(fallback_tick)
        if bridge_ticks or rest_ticks:
            return bridge_ticks + rest_ticks
        for exchange in self._venue_stats:
            fallback_tick = self._rest_fallback_tick(exchange, _FETCHERS.get(exchange))
            if fallback_tick is not None:
                rest_ticks.append(fallback_tick)
        return rest_ticks
    def _log(self, signal_info: dict[str, Any]) -> None:
        exchange = str(signal_info.get("exchange", "UNKNOWN")).upper()
        if exchange in self._venue_stats:
            self._venue_stats[exchange].ticks_used += 1
        sanitised_signal_info = _sanitise_signal_info(signal_info)
        self._signal_records.append(sanitised_signal_info)
        self._log_fp.write(json.dumps(sanitised_signal_info, allow_nan=False) + "\n")
        self._log_fp.flush()
    def _compute_ic_metrics(self) -> tuple[float, float]:
        sig_aligned, out_aligned = _build_signal_alignment(self._signal_records)
        if len(out_aligned) < 10 or out_aligned.std() <= 0 or sig_aligned.std() <= 0:
            return 0.0, 0.0
        ic = float(np.corrcoef(sig_aligned, out_aligned)[0, 1])
        if len(out_aligned) < 40:
            return ic, 0.0
        chunk = len(out_aligned) // 4
        ic_chunks = [
            float(np.corrcoef(sig_aligned[i * chunk : (i + 1) * chunk], out_aligned[i * chunk : (i + 1) * chunk])[0, 1])
            for i in range(4)
            if sig_aligned[i * chunk : (i + 1) * chunk].std() > 1e-9 and out_aligned[i * chunk : (i + 1) * chunk].std() > 1e-9
        ]
        if len(ic_chunks) < 2:
            return ic, 0.0
        ic_arr = np.asarray(ic_chunks)
        if ic_arr.std() <= 1e-9:
            return ic, 0.0
        return ic, float(ic_arr.mean() / ic_arr.std() * np.sqrt(252))
    def _print_summary(self) -> None:
        if not self._signal_records:
            print("  [shadow] No signals yet.")
            return
        raw_sigs_bps, effective_sigs_bps = _extract_signal_series(self._signal_records)
        ic, icir = self._compute_ic_metrics()
        diagnostics = _summarise_timestamp_quality(self._signal_records)
        churn = _summarise_regime_churn(self._signal_records)
        ts_quality = "warn" if diagnostics["has_timestamp_issues"] else "ok"
        now_steady_ns = time.monotonic_ns()
        summary = {
            "ticks": len(self._signal_records),
            "mean_effective_bps": float(np.mean(effective_sigs_bps)),
            "mean_raw_bps": float(np.mean(raw_sigs_bps)),
            "std_effective_bps": float(np.std(effective_sigs_bps)),
            "ic": float(ic),
            "icir": float(icir),
            "confidence_gate": self._gating_reason_counts["confidence_gate"],
            "horizon_gate": self._gating_reason_counts["horizon_disagreement_gate"],
            "safe_mode_gate": self._gating_reason_counts["safe_mode_gate"],
            "ts_quality": ts_quality,
            "seconds_since_last_continuous_train": float((now_steady_ns - self._last_continuous_train_steady_ns) / 1_000_000_000.0),
            "seconds_since_last_regime_train": float((now_steady_ns - self._last_regime_train_steady_ns) / 1_000_000_000.0),
            "dominant_regime": churn["dominant_regime"],
            "dominant_regime_change_count": int(churn["dominant_regime_change_count"]),
            "switches_per_minute": float(churn["switches_per_minute"]),
            "average_dominant_confidence": float(churn["average_dominant_confidence"]),
            "venue_health": {
                exchange: {
                    "health_state": stats.health_state,
                    "health_transition_count": stats.health_transition_count,
                    "health_transition_root_causes": stats.health_transition_root_causes,
                }
                for exchange, stats in self._venue_stats.items()
            },
            "retrain_failure_counts": dict(self._retrain_failure_counts),
            "regime_retrain_failure_counts": dict(self._regime_retrain_failure_counts),
        }
        print(f"[{_utcnow()}] [ShadowSummary] {json.dumps(summary, sort_keys=True)}")
    def _maybe_continuous_train(self) -> None:
        if self.cfg.continuous_train_every_ticks <= 0:
            return
        ticks_since_train = self._processed_ticks - self._last_continuous_train_tick
        if ticks_since_train < self.cfg.continuous_train_every_ticks:
            return
        now_steady_ns = time.monotonic_ns()
        elapsed_since_train_s = (now_steady_ns - self._last_continuous_train_steady_ns) / 1_000_000_000.0
        if elapsed_since_train_s < self.cfg.min_continuous_train_interval_s:
            return
        allowed, reason = self._continuous_retrain_guard()
        if not allowed:
            print(f"[{_utcnow()}] [Shadow] continuous retrain skipped — {reason}")
            return
        train_window = max(self.cfg.seq_len * 4, self.cfg.continuous_train_window_ticks)
        try:
            print(f"[{_utcnow()}] [Shadow] continuous retrain started processed_ticks={self._processed_ticks} window_ticks={train_window}")
            self.train_on_recent(train_window)
            self._last_continuous_train_tick = self._processed_ticks
            self._last_continuous_train_steady_ns = now_steady_ns
            print(f"[{_utcnow()}] [Shadow] continuous retrain completed processed_ticks={self._processed_ticks}")
        except Exception as exc:
            exc_class = type(exc).__name__
            self._retrain_failure_counts[exc_class] = self._retrain_failure_counts.get(exc_class, 0) + 1
            champion = self._registry.current_champion()
            context = {
                "processed_ticks": self._processed_ticks,
                "train_window_ticks": train_window,
                "continuous_train_every_ticks": self.cfg.continuous_train_every_ticks,
                "continuous_train_window_ticks": self.cfg.continuous_train_window_ticks,
                "incumbent_model_id": champion.get("model_id") if champion else None,
                "incumbent_model_path": champion.get("model_path") if champion else None,
            }
            tb = traceback.format_exc()
            print(
                f"[{_utcnow()}] [Shadow] continuous retrain failed"
                f" exc_class={exc_class} exc={exc}"
                f" context={json.dumps(context)}\n{tb}"
            )
    def _maybe_regime_retrain(self) -> None:
        if self.cfg.regime_retrain_every_ticks <= 0:
            return
        ticks_since_retrain = self._processed_ticks - self._last_regime_train_tick
        if ticks_since_retrain < self.cfg.regime_retrain_every_ticks:
            return
        now_steady_ns = time.monotonic_ns()
        session_elapsed_s = (now_steady_ns - self._session_steady_start_ns) / 1_000_000_000.0
        if session_elapsed_s < self.cfg.regime_startup_warmup_s:
            return
        elapsed_since_retrain_s = (now_steady_ns - self._last_regime_train_steady_ns) / 1_000_000_000.0
        if elapsed_since_retrain_s < self.cfg.min_regime_train_interval_s:
            return
        train_window = max(self.cfg.seq_len * 4, self.cfg.continuous_train_window_ticks)
        try:
            df = self._collect_training_ticks(train_window)
            self._train_regime_on_data(df)
            self._last_regime_train_tick = self._processed_ticks
            self._last_regime_train_steady_ns = now_steady_ns
        except Exception as exc:
            exc_class = type(exc).__name__
            self._regime_retrain_failure_counts[exc_class] = self._regime_retrain_failure_counts.get(exc_class, 0) + 1
            context = {
                "processed_ticks": self._processed_ticks,
                "train_window_ticks": train_window,
                "regime_retrain_every_ticks": self.cfg.regime_retrain_every_ticks,
            }
            tb = traceback.format_exc()
            print(
                f"[{_utcnow()}] [Shadow] regime retrain failed"
                f" exc_class={exc_class} exc={exc}"
                f" context={json.dumps(context)}\n{tb}"
            )
    def _update_quality_guards(self, signal_info: dict[str, Any]) -> None:
        if not self._signal_records:
            return
        previous_record = self._signal_records[-1]
        previous_mid = float(previous_record.get("mid_price", 0.0))
        if previous_mid <= 0:
            return
        realised = (float(signal_info["mid_price"]) - previous_mid) / previous_mid
        if self._drift_guard.update(float(previous_record.get("signal", 0.0)), realised):
            self._trigger_safe_mode(reason=f"drift_ic_breach ic={self._drift_guard.current_ic():.4f} floor={self.cfg.drift_ic_floor:.4f}")
        if self._canary is None or self._prev_primary_signal is None or self._prev_ensemble_signal is None:
            return
        if self._canary.update(self._prev_primary_signal, self._prev_ensemble_signal, realised):
            self._unload_secondary_model(
                reason=(
                    f"ic_degradation ensemble_ic={self._canary.ensemble_ic():.4f}"
                    f" primary_ic={self._canary.primary_ic():.4f}"
                    f" margin={self.cfg.canary_ic_margin}"
                )
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
                self._ring.extend(ticks)
                self._processed_ticks += len(ticks)
                if len(self._ring) > self._max_ring:
                    self._ring = self._ring[-self._max_ring :]
                self._maybe_continuous_train()
                self._maybe_regime_retrain()
                signal_info = self._infer()
                if signal_info is None:
                    self._publisher.publish(0.0, 0.0, 0.0, 0)
                else:
                    self._update_quality_guards(signal_info)
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
def _prepare_runtime_models(session: NeuralAlphaShadowSession, cfg: ShadowSessionConfig) -> None:
    primary_model_path = Path(cfg.model_path) if cfg.model_path else _symbol_model_path(cfg.symbol, "latest")
    secondary_model_path = Path(cfg.secondary_model_path) if cfg.secondary_model_path else _symbol_model_path(cfg.symbol, "secondary")
    if primary_model_path.exists():
        session.load_model(str(primary_model_path))
    if secondary_model_path.exists():
        session.load_secondary_model(str(secondary_model_path))
    if session._missing_model_stack() and cfg.train_ticks > 0:
        session.train_on_recent(cfg.train_ticks)
    missing = session._missing_model_stack()
    if not missing:
        session._validate_production_stack()
        return
    if session._model is None and cfg.require_full_model_stack:
        raise RuntimeError(
            "Research production stack requires a trained model. Collect more ticks before starting or adjust `session.train_ticks`/`session.require_full_model_stack` in YAML for non-production debugging."
        )
def main() -> None:
    import argparse

    parser = argparse.ArgumentParser(description="Neural Alpha Shadow Session")
    parser.add_argument("--signal-file", default=None)
    parser.add_argument("--lob-feed-path", default=None)
    parser.add_argument("--duration", type=int, default=None, dest="duration_s")
    parser.add_argument("--interval-ms", type=int, default=None)
    parser.add_argument("--report-interval", type=int, default=None, dest="report_interval_s")
    parser.add_argument("--seq-len", type=int, default=None)
    parser.add_argument("--symbol", default=None)
    parser.add_argument("--exchanges", default=None)
    parser.add_argument("--model-path", default=None)
    parser.add_argument("--secondary-model-path", default=None)
    parser.add_argument("--regime-model-path", default=None)
    parser.add_argument("--train-ticks", type=int, default=None)
    parser.add_argument("--train-epochs", type=int, default=None)
    parser.add_argument("--continuous-train-every-ticks", type=int, default=None)
    parser.add_argument("--continuous-train-window-ticks", type=int, default=None)
    parser.add_argument("--safe-mode-ticks", type=int, default=None)
    parser.add_argument("--drift-min-samples", type=int, default=None)
    parser.add_argument("--drift-ic-floor", type=float, default=None)
    parser.add_argument("--no-require-full-model-stack", action="store_true", default=False)
    parser.add_argument("--log-path", default=None)
    args = parser.parse_args()

    cfg = ShadowSessionConfig()
    if args.signal_file is not None:
        cfg.signal_file = args.signal_file
    if args.lob_feed_path is not None:
        cfg.lob_feed_path = args.lob_feed_path
    if args.duration_s is not None:
        cfg.duration_s = args.duration_s
    if args.interval_ms is not None:
        cfg.interval_ms = args.interval_ms
    if args.report_interval_s is not None:
        cfg.report_interval_s = args.report_interval_s
    if args.seq_len is not None:
        cfg.seq_len = args.seq_len
    if args.symbol is not None:
        cfg.symbol = args.symbol
    if args.exchanges is not None:
        cfg.exchanges = [e.strip().upper() for e in args.exchanges.split(",") if e.strip()]
    if args.model_path is not None:
        cfg.model_path = args.model_path
    if args.secondary_model_path is not None:
        cfg.secondary_model_path = args.secondary_model_path
    if args.regime_model_path is not None:
        cfg.regime_model_path = args.regime_model_path
    if args.train_ticks is not None:
        cfg.train_ticks = args.train_ticks
    if args.train_epochs is not None:
        cfg.train_epochs = args.train_epochs
    if args.continuous_train_every_ticks is not None:
        cfg.continuous_train_every_ticks = args.continuous_train_every_ticks
    if args.continuous_train_window_ticks is not None:
        cfg.continuous_train_window_ticks = args.continuous_train_window_ticks
    if args.safe_mode_ticks is not None:
        cfg.safe_mode_ticks = args.safe_mode_ticks
    if args.drift_min_samples is not None:
        cfg.drift_min_samples = args.drift_min_samples
    if args.drift_ic_floor is not None:
        cfg.drift_ic_floor = args.drift_ic_floor
    if args.no_require_full_model_stack:
        cfg.require_full_model_stack = False
    if args.log_path is not None:
        cfg.log_path = args.log_path

    session = NeuralAlphaShadowSession(cfg)
    def _handle_sigint(sig: int, frame: object) -> None:
        del sig, frame
        print("\nInterrupt received — stopping session...")
        session.stop()
    signal.signal(signal.SIGINT, _handle_sigint)
    _prepare_runtime_models(session, cfg)
    session.run()
if __name__ == "__main__":
    main()
