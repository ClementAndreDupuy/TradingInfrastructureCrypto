#!/usr/bin/env bash
# Shadow trading session — BTCUSDT perp / PI_XBTUSD futures.
#
# Runs two processes in parallel:
#   1. C++ perp_arb binary in shadow mode (order simulation via live books)
#   2. Python neural alpha shadow session (signal generation + IC tracking)
#
# Both processes write JSONL logs. daily_metrics.py reads these logs.
#
# Usage:
#   ./deploy/run_shadow.sh               # run for 24 h
#   ./deploy/run_shadow.sh --duration 3600  # run for 1 h
#
# Required environment:
#   BINANCE_API_KEY, BINANCE_API_SECRET  (read-only keys are fine for shadow)
#   KRAKEN_API_KEY,  KRAKEN_API_SECRET

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
VENV="$REPO_ROOT/.venv"
MODEL_PATH="$REPO_ROOT/models/neural_alpha_latest.pt"
DURATION="${1:---duration}"
DURATION_VAL="${2:-86400}"  # 24 h default

# Activate Python venv if it exists
if [[ -f "$VENV/bin/activate" ]]; then
    source "$VENV/bin/activate"
fi

# Build C++ binary if not built
if [[ ! -f "$BUILD_DIR/bin/perp_arb" ]]; then
    echo "[shadow] Building C++ binary…"
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF
    make -j"$(nproc)" perp_arb
    cd "$REPO_ROOT"
fi

echo "[shadow] Starting shadow session — duration=${DURATION_VAL}s"

# ── 1. C++ shadow engine ──────────────────────────────────────────────────────
LOG_TS=$(date +%s)
SHADOW_LOG="shadow_decisions_${LOG_TS}.jsonl"

"$BUILD_DIR/bin/perp_arb" \
    --shadow \
    --qty 0.001 \
    --mm-spread 6.0 \
    --taker-spread 12.0 \
    > "$REPO_ROOT/logs/perp_arb_shadow_${LOG_TS}.log" 2>&1 &
CPP_PID=$!
echo "[shadow] C++ perp_arb PID=$CPP_PID  log=$SHADOW_LOG"

# ── 2. Python neural alpha shadow session ────────────────────────────────────
ALPHA_ARGS=(
    --log-path  "$REPO_ROOT/neural_alpha_shadow.jsonl"
    --interval-ms 500
    --duration "$DURATION_VAL"
    --report-interval 60
    --seq-len 64
    --entry-bps 5.0
    --exchanges "BINANCE,KRAKEN"
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
    kill "$CPP_PID" "$PY_PID" 2>/dev/null || true
    wait "$CPP_PID" "$PY_PID" 2>/dev/null || true
    echo "[shadow] Generating daily metrics report…"
    python3 "$REPO_ROOT/deploy/daily_metrics.py"
    echo "[shadow] Done."
}
trap cleanup EXIT INT TERM

# Wait for the Python session (it respects --duration)
wait "$PY_PID"
