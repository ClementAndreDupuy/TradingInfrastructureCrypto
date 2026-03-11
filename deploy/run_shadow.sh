#!/usr/bin/env bash
# Shadow trading session — GNN neural alpha model on SOLUSDT (Binance spot).
#
# Runs the Python neural alpha shadow session to validate the GNN model
# before promoting to live.
#
# Usage:
#   ./deploy/run_shadow.sh               # run for 24 h
#   ./deploy/run_shadow.sh --duration 3600  # run for 1 h
#
# Required environment:
#   BINANCE_API_KEY, BINANCE_API_SECRET  (read-only keys are fine for shadow)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENV="$REPO_ROOT/.venv"
MODEL_PATH="$REPO_ROOT/models/neural_alpha_latest.pt"
DURATION="${1:---duration}"
DURATION_VAL="${2:-86400}"  # 24 h default

# Activate Python venv if it exists
if [[ -f "$VENV/bin/activate" ]]; then
    source "$VENV/bin/activate"
fi

echo "[shadow] Starting shadow session — duration=${DURATION_VAL}s"

LOG_TS=$(date +%s)

# ── GNN neural alpha shadow session (SOLUSDT) ─────────────────────────────────
ALPHA_ARGS=(
    --log-path  "$REPO_ROOT/neural_alpha_shadow.jsonl"
    --interval-ms 500
    --duration "$DURATION_VAL"
    --report-interval 60
    --seq-len 64
    --entry-bps 5.0
    --exchanges "SOLANA"
)

if [[ -f "$MODEL_PATH" ]]; then
    ALPHA_ARGS+=(--model-path "$MODEL_PATH")
    echo "[shadow] Loading model from $MODEL_PATH"
else
    ALPHA_ARGS+=(--train-ticks 500 --train-epochs 10)
    echo "[shadow] No model found — training on first 500 ticks"
fi

python3 -m research.alpha.neural_alpha.shadow_session "${ALPHA_ARGS[@]}" \
    > "$REPO_ROOT/logs/neural_alpha_shadow_${LOG_TS}.log" 2>&1 &
PY_PID=$!
echo "[shadow] Python shadow PID=$PY_PID"

# ── Cleanup on exit ──────────────────────────────────────────────────────────
cleanup() {
    echo "[shadow] Shutting down…"
    kill "$PY_PID" 2>/dev/null || true
    wait "$PY_PID" 2>/dev/null || true
    echo "[shadow] Generating daily metrics report…"
    python3 "$REPO_ROOT/deploy/daily_metrics.py"
    echo "[shadow] Done."
}
trap cleanup EXIT INT TERM

# Wait for the Python session (it respects --duration)
wait "$PY_PID"
