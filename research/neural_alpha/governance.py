from __future__ import annotations

import json
import time
import uuid
from collections import deque
from pathlib import Path
from typing import Any


class ChampionChallengerRegistry:
    """Lightweight model governance registry persisted as JSON."""

    def __init__(self, path: str | Path) -> None:
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)

    def _default_payload(self) -> dict[str, Any]:
        return {
            "champion_id": None,
            "models": [],
            "events": [],
        }

    def _read(self) -> dict[str, Any]:
        if not self.path.exists():
            return self._default_payload()
        with open(self.path) as f:
            payload = json.load(f)
        payload.setdefault("champion_id", None)
        payload.setdefault("models", [])
        payload.setdefault("events", [])
        return payload

    def _write(self, payload: dict[str, Any]) -> None:
        tmp = self.path.with_suffix(self.path.suffix + ".tmp")
        with open(tmp, "w") as f:
            json.dump(payload, f, indent=2)
        tmp.replace(self.path)

    def register_challenger(self, model_path: str | Path, metrics: dict[str, Any]) -> str:
        payload = self._read()
        model_id = f"model_{int(time.time())}_{uuid.uuid4().hex[:8]}"
        payload["models"].append(
            {
                "model_id": model_id,
                "model_path": str(model_path),
                "status": "challenger",
                "metrics": metrics,
                "created_at_ns": time.time_ns(),
            }
        )
        payload["events"].append(
            {
                "event": "register_challenger",
                "model_id": model_id,
                "timestamp_ns": time.time_ns(),
            }
        )
        self._write(payload)
        return model_id

    def promote(self, model_id: str, reason: str) -> None:
        payload = self._read()
        previous = payload.get("champion_id")

        for model in payload["models"]:
            if model["model_id"] == previous:
                model["status"] = "retired"
            if model["model_id"] == model_id:
                model["status"] = "champion"
                model["promoted_at_ns"] = time.time_ns()

        payload["champion_id"] = model_id
        payload["events"].append(
            {
                "event": "promote",
                "model_id": model_id,
                "previous_champion_id": previous,
                "reason": reason,
                "timestamp_ns": time.time_ns(),
            }
        )
        self._write(payload)

    def current_champion(self) -> dict[str, Any] | None:
        payload = self._read()
        cid = payload.get("champion_id")
        if cid is None:
            return None
        return next((m for m in payload["models"] if m["model_id"] == cid), None)

    def rollback_to_previous_champion(self, reason: str) -> str | None:
        payload = self._read()
        events = [e for e in payload["events"] if e.get("event") == "promote"]
        if not events:
            return None

        last_promote = events[-1]
        previous_id = last_promote.get("previous_champion_id")
        if not previous_id:
            return None

        for model in payload["models"]:
            if model["model_id"] == payload.get("champion_id"):
                model["status"] = "rolled_back"
            if model["model_id"] == previous_id:
                model["status"] = "champion"

        payload["champion_id"] = previous_id
        payload["events"].append(
            {
                "event": "rollback",
                "from_model_id": last_promote.get("model_id"),
                "to_model_id": previous_id,
                "reason": reason,
                "timestamp_ns": time.time_ns(),
            }
        )
        self._write(payload)
        champion = next((m for m in payload["models"] if m["model_id"] == previous_id), None)
        return None if champion is None else champion.get("model_path")


class DriftGuard:
    """Tracks rolling realised IC and signals when safe mode should be enabled."""

    def __init__(self, window: int = 200, min_samples: int = 60, ic_floor: float = -0.05) -> None:
        self.window = window
        self.min_samples = min_samples
        self.ic_floor = ic_floor
        self._signals: deque[float] = deque(maxlen=window)
        self._outcomes: deque[float] = deque(maxlen=window)

    def update(self, signal: float, outcome: float) -> bool:
        self._signals.append(float(signal))
        self._outcomes.append(float(outcome))
        ic = self.current_ic()
        return ic is not None and ic < self.ic_floor

    def current_ic(self) -> float | None:
        if len(self._signals) < self.min_samples:
            return None
        import numpy as np

        sig = np.asarray(self._signals, dtype=np.float64)
        out = np.asarray(self._outcomes, dtype=np.float64)
        if sig.std() < 1e-9 or out.std() < 1e-9:
            return None
        return float(np.corrcoef(sig, out)[0, 1])
