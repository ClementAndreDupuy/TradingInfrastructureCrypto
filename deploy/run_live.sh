#!/usr/bin/env bash
# Live trading — BTCUSDT perp / PI_XBTUSD futures.
#
# SAFETY CHECKLIST (must complete before running this script):
#   [ ] Shadow trading ran >= 2 weeks without issues
#   [ ] Shadow fill rates within 10% of backtest
#   [ ] Kill switch drill completed successfully
#   [ ] All Prometheus/Grafana dashboards operational
#   [ ] config/live/risk.yaml reviewed and limits set
#   [ ] API keys are LIVE keys (not testnet)
#   [ ] Daily training job running (deploy/daily_train.py in cron)
#
# Usage:
#   ./deploy/run_live.sh
#
# Required environment variables (never hardcode — load from secrets.yaml or vault):
#   BINANCE_API_KEY, BINANCE_API_SECRET
#   KRAKEN_API_KEY,  KRAKEN_API_SECRET

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
VENV="$REPO_ROOT/.venv"
MODEL_PATH="$REPO_ROOT/models/neural_alpha_latest.pt"

# ── Pre-flight checks ─────────────────────────────────────────────────────────
echo "[live] Running pre-flight checks…"

if [[ -z "${BINANCE_API_KEY:-}" || -z "${BINANCE_API_SECRET:-}" ]]; then
    echo "[live] ERROR: BINANCE_API_KEY / BINANCE_API_SECRET not set."
    exit 1
fi

if [[ -z "${KRAKEN_API_KEY:-}" || -z "${KRAKEN_API_SECRET:-}" ]]; then
    echo "[live] ERROR: KRAKEN_API_KEY / KRAKEN_API_SECRET not set."
    exit 1
fi

if [[ ! -f "$MODEL_PATH" ]]; then
    echo "[live] ERROR: No production model at $MODEL_PATH. Run daily_train.py first."
    exit 1
fi

if [[ ! -f "$BUILD_DIR/bin/perp_arb" ]]; then
    echo "[live] Building C++ binary in Release mode…"
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF
    make -j"$(nproc)" perp_arb
    cd "$REPO_ROOT"
fi

# Activate Python venv
if [[ -f "$VENV/bin/activate" ]]; then
    source "$VENV/bin/activate"
fi

LOG_TS=$(date +%s)
mkdir -p "$REPO_ROOT/logs"

echo "[live] All checks passed. Starting live session — $(date -u '+%Y-%m-%dT%H:%M:%SZ')"

# ── 1. C++ hot path (live mode) ───────────────────────────────────────────────
"$BUILD_DIR/bin/perp_arb" \
    --live \
    --qty 0.001 \
    --mm-spread 6.0 \
    --taker-spread 12.0 \
    > "$REPO_ROOT/logs/perp_arb_live_${LOG_TS}.log" 2>&1 &
CPP_PID=$!
echo "[live] C++ perp_arb PID=$CPP_PID"

# ── 2. Neural alpha signal server (read-only, no order submission) ────────────
python3 -m research.alpha.neural_alpha.shadow_session \
    --model-path "$MODEL_PATH" \
    --log-path "$REPO_ROOT/neural_alpha_live.jsonl" \
    --interval-ms 500 \
    --duration 86400 \
    --report-interval 60 \
    --seq-len 64 \
    --exchanges "BINANCE,KRAKEN" \
    > "$REPO_ROOT/logs/neural_alpha_live_${LOG_TS}.log" 2>&1 &
PY_PID=$!
echo "[live] Neural alpha PID=$PY_PID"

# ── Cleanup on exit ──────────────────────────────────────────────────────────
cleanup() {
    echo "[live] Shutdown signal received — stopping…"
    kill "$CPP_PID" "$PY_PID" 2>/dev/null || true
    wait "$CPP_PID" "$PY_PID" 2>/dev/null || true
    echo "[live] Writing final metrics…"
    python3 "$REPO_ROOT/deploy/daily_metrics.py"
    echo "[live] Session ended — $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
}
trap cleanup EXIT INT TERM

wait "$CPP_PID"
