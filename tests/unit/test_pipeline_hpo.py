from __future__ import annotations

import polars as pl
import pytest

from research.neural_alpha.models.trainer import TrainerConfig
from research.neural_alpha.pipeline import (
    _auto_tune_trainer_config,
    _candidate_dicts,
    _validate_alpha_input_schema,
)


def test_candidate_dicts_respects_max_trials() -> None:
    search_space = {"lr": [1e-4, 2e-4], "dropout": [0.1, 0.2], "batch_size": [32, 64]}
    trials = _candidate_dicts(search_space, max_trials=3)
    assert len(trials) == 3
    assert trials[0] == {"lr": 1e-4, "dropout": 0.1, "batch_size": 32}


def test_auto_tune_trainer_config_selects_best_trial(monkeypatch) -> None:
    def fake_model_cfg() -> dict:
        return {
            "search": {
                "enabled": True,
                "tuning_epochs": 3,
                "max_trials": 4,
                "primary": {
                    "lr": [1e-4, 2e-4],
                    "dropout": [0.1, 0.2],
                },
            }
        }

    def fake_walk_forward_train(df: pl.DataFrame, cfg: TrainerConfig) -> list[dict]:
        score = 1.0 / cfg.lr + cfg.dropout * 10.0
        if cfg.lr == 2e-4 and cfg.dropout == 0.1:
            score = 10.0
        return [{"best_selection_score": score}]

    monkeypatch.setattr("research._config.model_cfg", fake_model_cfg)
    monkeypatch.setattr("research.neural_alpha.models.trainer.walk_forward_train", fake_walk_forward_train)

    base_cfg = TrainerConfig(epochs=9, verbose=True, lr=3e-4, dropout=0.3)
    selected = _auto_tune_trainer_config(
        pl.DataFrame({"timestamp_ns": [1, 2, 3], "best_bid": [1.0, 1.0, 1.0], "best_ask": [1.1, 1.1, 1.1]}),
        base_cfg,
        secondary=False,
        enabled=True,
    )

    assert selected.lr == 2e-4
    assert selected.dropout == 0.1
    assert selected.epochs == 9
    assert selected.verbose is True


def test_auto_tune_accepts_string_large_selection_score(monkeypatch) -> None:
    def fake_pipeline_cfg() -> dict:
        return {
            "n_levels": 10,
            "default_exchanges": ["BINANCE"],
            "urls": {
                "binance_depth": "https://example.com/binance",
                "kraken_depth": "https://example.com/kraken",
                "okx_depth": "https://example.com/okx",
                "coinbase_depth": "https://example.com/coinbase",
            },
            "large_selection_score": "1.0e9",
            "request_timeout_s": 5,
            "holdout_frac": 0.8,
        }

    def fake_model_cfg() -> dict:
        return {
            "search": {
                "enabled": True,
                "tuning_epochs": 2,
                "max_trials": 1,
                "primary": {"lr": [2e-4]},
            }
        }

    def fake_walk_forward_train(df: pl.DataFrame, cfg: TrainerConfig) -> list[dict]:
        return [{"best_selection_score": 0.5}]

    monkeypatch.setattr("research._config.pipeline_cfg", fake_pipeline_cfg)
    monkeypatch.setattr("research._config.model_cfg", fake_model_cfg)
    monkeypatch.setattr("research.neural_alpha.models.trainer.walk_forward_train", fake_walk_forward_train)
    monkeypatch.setattr("research.neural_alpha.models.trainer.TrainerConfig", TrainerConfig)

    from importlib import reload
    from research.neural_alpha import pipeline as pipeline_module

    reloaded = reload(pipeline_module)
    base_cfg = TrainerConfig(epochs=4, verbose=True, lr=3e-4)
    selected = reloaded._auto_tune_trainer_config(
        pl.DataFrame({"timestamp_ns": [1, 2, 3], "best_bid": [1.0, 1.0, 1.0], "best_ask": [1.1, 1.1, 1.1]}),
        base_cfg,
        secondary=False,
        enabled=True,
    )

    assert isinstance(reloaded.LARGE_SELECTION_SCORE, float)
    assert selected.lr == 2e-4


def test_validate_alpha_input_schema_accepts_enriched_lob_data() -> None:
    rows = []
    for idx in range(4):
        row = {
            "timestamp_ns": idx + 1,
            "exchange": "BINANCE",
            "symbol": "BTCUSDT",
            "best_bid": 100.0,
            "best_ask": 100.1,
            "last_trade_price": 100.05,
            "last_trade_size": 0.2,
            "recent_traded_volume": 0.3,
            "trade_direction": 1,
        }
        for level in range(1, 11):
            row[f"bid_price_{level}"] = 100.0 - level * 0.01
            row[f"ask_price_{level}"] = 100.1 + level * 0.01
            row[f"bid_size_{level}"] = 1.0
            row[f"ask_size_{level}"] = 1.0
            row[f"bid_oc_{level}"] = 2 + level
            row[f"ask_oc_{level}"] = 3 + level
        rows.append(row)
    _validate_alpha_input_schema(pl.DataFrame(rows))


def test_validate_alpha_input_schema_rejects_missing_count_columns() -> None:
    rows = []
    for idx in range(4):
        row = {
            "timestamp_ns": idx + 1,
            "best_bid": 100.0,
            "best_ask": 100.1,
            "last_trade_price": 100.05,
            "last_trade_size": 0.2,
            "recent_traded_volume": 0.3,
            "trade_direction": 1,
        }
        for level in range(1, 11):
            row[f"bid_price_{level}"] = 100.0 - level * 0.01
            row[f"ask_price_{level}"] = 100.1 + level * 0.01
            row[f"bid_size_{level}"] = 1.0
            row[f"ask_size_{level}"] = 1.0
        rows.append(row)
    with pytest.raises(RuntimeError, match="order-count columns"):
        _validate_alpha_input_schema(pl.DataFrame(rows))
