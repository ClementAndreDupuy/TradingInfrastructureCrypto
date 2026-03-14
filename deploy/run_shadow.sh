#!/usr/bin/env bash
# Shadow trading runner — four-venue orchestration for local validation.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENGINE_BIN="${ENGINE_BIN:-$REPO_ROOT/build/bin/trading_engine}"
ENV_FILE_DEFAULT="$REPO_ROOT/config/shadow/trading.env"

usage() {
    cat <<USAGE
Usage: ./deploy/run_shadow.sh [options] [-- <extra trading_engine args>]

Options:
  --symbol <SYMBOL>         Trading symbol (default: BTCUSDT)
  --venues <CSV>            Venues CSV (default: BINANCE,KRAKEN,OKX,COINBASE)
  --duration <SECONDS>      Loop duration in seconds (default: 60)
  --interval-ms <MS>        Delay between engine runs (default: 1000)
  --env-file <PATH>         Optional env file to source (default: config/shadow/trading.env if present)
  --once                    Run exactly one iteration and exit
  -h, --help                Show this help

Credential resolution per venue (first non-empty wins):
  SHADOW_<VENUE>_API_KEY, <VENUE>_API_KEY, fallback=local-shadow-key
  SHADOW_<VENUE>_API_SECRET, <VENUE>_API_SECRET, fallback=local-shadow-secret
USAGE
}

SYMBOL="BTCUSDT"
VENUES="BINANCE,KRAKEN,OKX,COINBASE"
DURATION_SECS=60
INTERVAL_MS=1000
RUN_ONCE=0
ENV_FILE="$ENV_FILE_DEFAULT"
EXTRA_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --symbol) SYMBOL="$2"; shift 2 ;;
        --venues) VENUES="$2"; shift 2 ;;
        --duration) DURATION_SECS="$2"; shift 2 ;;
        --interval-ms) INTERVAL_MS="$2"; shift 2 ;;
        --env-file) ENV_FILE="$2"; shift 2 ;;
        --once) RUN_ONCE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        --) shift; EXTRA_ARGS=("$@"); break ;;
        *) echo "[shadow] Unknown argument: $1"; usage; exit 1 ;;
    esac
done

if [[ -f "$ENV_FILE" ]]; then
    echo "[shadow] Loading env file: $ENV_FILE"
    # shellcheck disable=SC1090
    source "$ENV_FILE"
fi

if [[ ! -x "$ENGINE_BIN" ]]; then
    echo "[shadow] ERROR: missing trading_engine binary at $ENGINE_BIN"
    echo "[shadow] Build first: mkdir -p build && cd build && cmake .. && make -j\$(nproc)"
    exit 1
fi

set_shadow_creds() {
    local venue="$1"
    local key_var="${venue}_API_KEY"
    local secret_var="${venue}_API_SECRET"
    local shadow_key_var="SHADOW_${venue}_API_KEY"
    local shadow_secret_var="SHADOW_${venue}_API_SECRET"

    local key_val="${!shadow_key_var:-${!key_var:-local-shadow-key}}"
    local secret_val="${!shadow_secret_var:-${!secret_var:-local-shadow-secret}}"

    export "$key_var=$key_val"
    export "$secret_var=$secret_val"
}

IFS=',' read -r -a VENUE_LIST <<< "$VENUES"
for venue in "${VENUE_LIST[@]}"; do
    set_shadow_creds "$venue"
done

run_engine_once() {
    "$ENGINE_BIN" --mode shadow --venues "$VENUES" --symbol "$SYMBOL" "${EXTRA_ARGS[@]}"
}

echo "[shadow] Starting shadow runner symbol=$SYMBOL venues=$VENUES duration=${DURATION_SECS}s interval=${INTERVAL_MS}ms"

if [[ "$RUN_ONCE" -eq 1 ]]; then
    run_engine_once
    exit 0
fi

end_epoch=$(( $(date +%s) + DURATION_SECS ))
while [[ $(date +%s) -lt $end_epoch ]]; do
    run_engine_once
    sleep "$(awk "BEGIN { printf \"%.3f\", $INTERVAL_MS/1000 }")"
done

echo "[shadow] Completed shadow run window"
