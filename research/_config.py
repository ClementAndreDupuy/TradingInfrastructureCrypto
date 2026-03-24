from __future__ import annotations

from functools import lru_cache
from pathlib import Path

import yaml

_ROOT = Path(__file__).resolve().parent.parent / "config" / "research"


@lru_cache(maxsize=None)
def _load(name: str) -> dict:
    with open(_ROOT / f"{name}.yaml", encoding="utf-8") as fh:
        return yaml.safe_load(fh)


def pipeline_cfg() -> dict:
    return _load("pipeline")


def regime_cfg() -> dict:
    return _load("regime")


def model_cfg() -> dict:
    return _load("model")


def shadow_cfg() -> dict:
    return _load("shadow")
