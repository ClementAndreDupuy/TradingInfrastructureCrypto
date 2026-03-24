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


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Walk-forward regime stability backtest")
    parser.add_argument("--ipc-dir", type=str, required=True, help="Directory containing parquet/csv LOB files")
    parser.add_argument("--output", type=str, default="models/regime_backtest_summary.json")
    parser.add_argument("--n-regimes", type=int, default=4)
    parser.add_argument("--max-iter", type=int, default=75)
    parser.add_argument("--tol", type=float, default=1e-4)
    parser.add_argument("--train-window", type=int, default=20_000)
    parser.add_argument("--test-window", type=int, default=5_000)
    parser.add_argument("--step", type=int, default=5_000)
    parser.add_argument("--min-confidence", type=float, default=0.60)
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
