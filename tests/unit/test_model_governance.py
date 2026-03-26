from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).parent.parent.parent
sys.path.insert(0, str(ROOT))

from research.neural_alpha.operations.governance import ChampionChallengerRegistry, DriftGuard, EnsembleCanary


def test_registry_register_and_promote(tmp_path: Path) -> None:
    registry_path = tmp_path / "model_registry.json"
    registry = ChampionChallengerRegistry(registry_path)

    first = registry.register_challenger("models/a.pt", {"ic_mean": 0.02})
    registry.promote(first, reason="bootstrap")
    champ = registry.current_champion()

    assert champ is not None
    assert champ["model_id"] == first
    assert champ["status"] == "champion"


def test_registry_rollback_to_previous_champion(tmp_path: Path) -> None:
    registry = ChampionChallengerRegistry(tmp_path / "registry.json")
    first = registry.register_challenger("models/champion_a.pt", {"ic_mean": 0.03})
    registry.promote(first, reason="seed")

    second = registry.register_challenger("models/champion_b.pt", {"ic_mean": 0.04})
    registry.promote(second, reason="better")

    rollback_path = registry.rollback_to_previous_champion(reason="drift")
    champion = registry.current_champion()

    assert rollback_path == "models/champion_a.pt"
    assert champion is not None
    assert champion["model_id"] == first


def test_drift_guard_triggers_when_ic_breaches_floor() -> None:
    guard = DriftGuard(window=80, min_samples=40, ic_floor=-0.1)
    rng = np.random.default_rng(7)
    signal = rng.normal(0, 1, size=80)
    outcome = -signal + rng.normal(0, 0.05, size=80)

    triggered = False
    for s, o in zip(signal, outcome):
        triggered = guard.update(float(s), float(o)) or triggered

    assert triggered
    ic = guard.current_ic()
    assert ic is not None and ic < -0.1


def test_drift_guard_requires_sustained_breach() -> None:
    guard = DriftGuard(window=40, min_samples=10, ic_floor=-0.05, breach_streak_required=4)
    rng = np.random.default_rng(42)
    signal = rng.normal(0, 1, size=40)
    outcome = -signal + rng.normal(0, 0.01, size=40)

    triggered_count = 0
    for s, o in zip(signal, outcome):
        if guard.update(float(s), float(o)):
            triggered_count += 1

    assert triggered_count == 1


def test_ensemble_canary_requires_streak_before_rollback() -> None:
    canary = EnsembleCanary(
        window=100,
        min_samples=30,
        ic_margin=0.05,
        trigger_streak_required=5,
        rearm_streak_required=3,
    )
    rng = np.random.default_rng(123)
    base = rng.normal(0, 1, size=120)
    primary = base + rng.normal(0, 0.01, size=120)
    ensemble = -base + rng.normal(0, 0.05, size=120)

    triggered_count = 0
    for p, e, o in zip(primary, ensemble, base):
        if canary.update(float(p), float(e), float(o)):
            triggered_count += 1

    assert triggered_count == 1
