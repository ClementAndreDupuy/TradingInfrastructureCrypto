#!/usr/bin/env bash
# Live trading — GNN neural alpha model on SOLUSDT (Binance spot).
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

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENV="$REPO_ROOT/.venv"
MODEL_PATH="$REPO_ROOT/models/neural_alpha_latest.pt"

# ── Pre-flight checks ─────────────────────────────────────────────────────────
echo "[live] Running pre-flight checks…"

if [[ -z "${BINANCE_API_KEY:-}" || -z "${BINANCE_API_SECRET:-}" ]]; then
    echo "[live] ERROR: BINANCE_API_KEY / BINANCE_API_SECRET not set."
    exit 1
fi

if [[ ! -f "$MODEL_PATH" ]]; then
    echo "[live] ERROR: No production model at $MODEL_PATH. Run daily_train.py first."
    exit 1
fi

# Activate Python venv
if [[ -f "$VENV/bin/activate" ]]; then
    source "$VENV/bin/activate"
fi

LOG_TS=$(date +%s)
mkdir -p "$REPO_ROOT/logs"

echo "[live] All checks passed. Starting live session — $(date -u '+%Y-%m-%dT%H:%M:%SZ')"

# ── GNN neural alpha live session (SOLUSDT) ───────────────────────────────────
python3 -m research.alpha.neural_alpha.shadow_session \
    --model-path "$MODEL_PATH" \
    --log-path "$REPO_ROOT/neural_alpha_live.jsonl" \
    --interval-ms 500 \
    --duration 86400 \
    --report-interval 60 \
    --seq-len 64 \
    --exchanges "SOLANA" \
    > "$REPO_ROOT/logs/neural_alpha_live_${LOG_TS}.log" 2>&1 &
PY_PID=$!
echo "[live] Neural alpha (SOLANA) PID=$PY_PID"

# ── Cleanup on exit ──────────────────────────────────────────────────────────
cleanup() {
    echo "[live] Shutdown signal received — stopping…"
    kill "$PY_PID" 2>/dev/null || true
    wait "$PY_PID" 2>/dev/null || true
    echo "[live] Writing final metrics…"
    python3 "$REPO_ROOT/deploy/daily_metrics.py"
    echo "[live] Session ended — $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
}
trap cleanup EXIT INT TERM

wait "$PY_PID"
