from __future__ import annotations

from functools import lru_cache
from pathlib import Path
from typing import Any, Callable, TypeVar, cast

import yaml

_ROOT = Path(__file__).resolve().parent.parent / "config" / "research"
_T = TypeVar("_T")


class ConfigValidationError(RuntimeError):
    """Raised when a research config contains an invalid field value."""


def _require_mapping(value: Any, *, field: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ConfigValidationError(f"{field} must be a mapping, got {type(value).__name__}")
    return cast(dict[str, Any], value)


def _coerce_scalar(
    value: Any,
    *,
    field: str,
    expected: type[_T],
    parser: Callable[[Any], _T],
) -> _T:
    if isinstance(value, expected):
        return value
    try:
        return parser(value)
    except (TypeError, ValueError) as exc:
        raise ConfigValidationError(
            f"{field} must be {expected.__name__}, got {value!r} ({type(value).__name__})"
        ) from exc


def _as_int(value: Any, *, field: str) -> int:
    return _coerce_scalar(value, field=field, expected=int, parser=int)


def _as_float(value: Any, *, field: str) -> float:
    return _coerce_scalar(value, field=field, expected=float, parser=float)


def _as_bool(value: Any, *, field: str) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, str):
        lowered = value.strip().lower()
        if lowered in {"true", "1", "yes", "on"}:
            return True
        if lowered in {"false", "0", "no", "off"}:
            return False
    raise ConfigValidationError(f"{field} must be bool, got {value!r} ({type(value).__name__})")


def _normalize_pipeline(raw: dict[str, Any]) -> dict[str, Any]:
    cfg = dict(raw)
    cfg["n_levels"] = _as_int(cfg.get("n_levels"), field="pipeline.n_levels")
    cfg["request_timeout_s"] = _as_int(cfg.get("request_timeout_s"), field="pipeline.request_timeout_s")
    cfg["holdout_frac"] = _as_float(cfg.get("holdout_frac"), field="pipeline.holdout_frac")
    cfg["large_selection_score"] = _as_float(
        cfg.get("large_selection_score"), field="pipeline.large_selection_score"
    )
    if "collection" in cfg:
        collection = _require_mapping(cfg["collection"], field="pipeline.collection")
        collection["ticks"] = _as_int(collection.get("ticks"), field="pipeline.collection.ticks")
        collection["interval_ms"] = _as_int(collection.get("interval_ms"), field="pipeline.collection.interval_ms")
        cfg["collection"] = collection
    return cfg


def _normalize_search_space(search_space: Any, *, field: str) -> dict[str, Any]:
    search = _require_mapping(search_space, field=field)
    normalized = dict(search)
    if "enabled" in normalized:
        normalized["enabled"] = _as_bool(normalized["enabled"], field=f"{field}.enabled")
    if "max_trials" in normalized:
        normalized["max_trials"] = _as_int(normalized["max_trials"], field=f"{field}.max_trials")
    if "tuning_epochs" in normalized:
        normalized["tuning_epochs"] = _as_int(normalized["tuning_epochs"], field=f"{field}.tuning_epochs")

    int_lists = {"batch_size"}
    for model_name in ("primary", "secondary"):
        model_search = normalized.get(model_name)
        if model_search is None:
            continue
        model_search_map = _require_mapping(model_search, field=f"{field}.{model_name}")
        model_search_normalized: dict[str, Any] = {}
        for key, values in model_search_map.items():
            if not isinstance(values, list):
                raise ConfigValidationError(f"{field}.{model_name}.{key} must be a list")
            if key in int_lists:
                model_search_normalized[key] = [
                    _as_int(item, field=f"{field}.{model_name}.{key}[{idx}]")
                    for idx, item in enumerate(values)
                ]
            else:
                model_search_normalized[key] = [
                    _as_float(item, field=f"{field}.{model_name}.{key}[{idx}]")
                    for idx, item in enumerate(values)
                ]
        normalized[model_name] = model_search_normalized
    return normalized


def _normalize_model(raw: dict[str, Any]) -> dict[str, Any]:
    cfg = dict(raw)
    search = cfg.get("search")
    if search is not None:
        cfg["search"] = _normalize_search_space(search, field="model.search")
    return cfg


@lru_cache(maxsize=None)
def _load(name: str) -> dict:
    with open(_ROOT / f"{name}.yaml", encoding="utf-8") as fh:
        raw = yaml.safe_load(fh)
    if not isinstance(raw, dict):
        raise ConfigValidationError(f"{name} config must be a mapping")
    if name == "pipeline":
        return _normalize_pipeline(raw)
    if name == "model":
        return _normalize_model(raw)
    return raw


def pipeline_cfg() -> dict:
    return _load("pipeline")


def regime_cfg() -> dict:
    return _load("regime")


def model_cfg() -> dict:
    return _load("model")


def shadow_cfg() -> dict:
    return _load("shadow")
