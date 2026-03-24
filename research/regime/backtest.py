from __future__ import annotations

import argparse
import json
from pathlib import Path

from .regime import (
    RegimeBacktestConfig,
    RegimeConfig,
    _load_ipc_lob_frame,
    run_regime_walk_forward_backtest,
)
from .._config import regime_cfg

_rcfg = regime_cfg()


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Walk-forward regime stability backtest")
    parser.add_argument("--ipc-dir", type=str, required=True, help="Directory containing parquet/csv LOB files")
    parser.add_argument("--output", type=str, default=_rcfg["backtest"]["output"])
    parser.add_argument("--n-regimes", type=int, default=_rcfg["model"]["n_regimes"])
    parser.add_argument("--max-iter", type=int, default=_rcfg["model"]["max_iter"])
    parser.add_argument("--tol", type=float, default=_rcfg["model"]["tol"])
    parser.add_argument("--train-window", type=int, default=_rcfg["backtest"]["train_window"])
    parser.add_argument("--test-window", type=int, default=_rcfg["backtest"]["test_window"])
    parser.add_argument("--step", type=int, default=_rcfg["backtest"]["step"])
    parser.add_argument("--min-confidence", type=float, default=_rcfg["backtest"]["min_confidence"])
    return parser


def main() -> None:
    args = _build_parser().parse_args()
    frame = _load_ipc_lob_frame(args.ipc_dir)
    summary = run_regime_walk_forward_backtest(
        frame,
        RegimeConfig(n_regimes=args.n_regimes, max_iter=args.max_iter, tol=args.tol),
        RegimeBacktestConfig(
            train_window=args.train_window,
            test_window=args.test_window,
            step=args.step,
            min_confidence=args.min_confidence,
        ),
    )
    payload = {
        "windows": summary.windows,
        "samples": summary.samples,
        "mean_confidence": summary.mean_confidence,
        "mean_entropy": summary.mean_entropy,
        "dominant_switch_rate": summary.dominant_switch_rate,
        "low_confidence_rate": summary.low_confidence_rate,
    }
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    print(json.dumps(payload, indent=2))


if __name__ == "__main__":
    main()
