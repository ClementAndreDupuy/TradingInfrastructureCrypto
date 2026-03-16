#!/usr/bin/env bash
# Live trading — four-venue SOR runtime (Binance, Kraken, OKX, Coinbase).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENGINE_BIN="${ENGINE_BIN:-$REPO_ROOT/build/bin/trading_engine}"
ENV_FILE_DEFAULT="$REPO_ROOT/config/live/trading.env"
CONFIG_FILE_DEFAULT="$REPO_ROOT/config/live/runtime.yaml"

read_yaml_value() {
    local file_path="$1"
    local key="$2"
    awk -F: -v search_key="$key" '
        $1 ~ "^[[:space:]]*" search_key "[[:space:]]*$" {
            value = $0
            sub(/^[^:]*:[[:space:]]*/, "", value)
            sub(/[[:space:]]+#.*/, "", value)
            gsub(/^["\047]|["\047]$/, "", value)
            print value
            exit
        }
    ' "$file_path"
}

resolve_config_path() {
    local raw_path="$1"
    if [[ "$raw_path" = /* ]]; then
        echo "$raw_path"
    else
        echo "$REPO_ROOT/$raw_path"
    fi
}

apply_config_defaults() {
    if [[ ! -f "$CONFIG_FILE" ]]; then
        return
    fi

    local symbol_from_cfg venues_from_cfg interval_from_cfg env_from_cfg
    symbol_from_cfg="$(read_yaml_value "$CONFIG_FILE" "symbol")"
    venues_from_cfg="$(read_yaml_value "$CONFIG_FILE" "venues")"
    interval_from_cfg="$(read_yaml_value "$CONFIG_FILE" "interval_ms")"
    env_from_cfg="$(read_yaml_value "$CONFIG_FILE" "env_file")"

    [[ -n "$symbol_from_cfg" ]] && SYMBOL="$symbol_from_cfg"
    [[ -n "$venues_from_cfg" ]] && VENUES="$venues_from_cfg"
    [[ -n "$interval_from_cfg" ]] && LOOP_INTERVAL_MS="$interval_from_cfg"
    [[ -n "$env_from_cfg" ]] && ENV_FILE="$(resolve_config_path "$env_from_cfg")"
}

usage() {
    cat <<USAGE
Usage: ./deploy/run_live.sh [options] [-- <extra trading_engine args>]

Options:
  --config <PATH>         YAML runtime defaults (default: config/live/runtime.yaml if present)
  --symbol <SYMBOL>       Trading symbol (default: BTCUSDT)
  --venues <CSV>          Venues CSV (default: BINANCE,KRAKEN,OKX,COINBASE)
  --interval-ms <MS>      Engine loop interval in milliseconds (default: 500)
  --env-file <PATH>       Optional env file (default: config/live/trading.env if present)
  -h, --help              Show this help

For each venue, live credentials must be present:
  LIVE_<VENUE>_API_KEY or <VENUE>_API_KEY
  LIVE_<VENUE>_API_SECRET or <VENUE>_API_SECRET
USAGE
}

SYMBOL="BTCUSDT"
VENUES="BINANCE,KRAKEN,OKX,COINBASE"
LOOP_INTERVAL_MS=500
ENV_FILE="$ENV_FILE_DEFAULT"
CONFIG_FILE="$CONFIG_FILE_DEFAULT"
EXTRA_ARGS=()

for ((i = 1; i <= $#; i++)); do
    if [[ "${!i}" == "--config" ]]; then
        next_idx=$((i + 1))
        CONFIG_FILE="$(resolve_config_path "${!next_idx}")"
        break
    fi
done

apply_config_defaults

while [[ $# -gt 0 ]]; do
    case "$1" in
        --config) CONFIG_FILE="$(resolve_config_path "$2")"; shift 2 ;;
        --symbol) SYMBOL="$2"; shift 2 ;;
        --venues) VENUES="$2"; shift 2 ;;
        --interval-ms) LOOP_INTERVAL_MS="$2"; shift 2 ;;
        --env-file) ENV_FILE="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        --) shift; EXTRA_ARGS=("$@"); break ;;
        *) echo "[live] Unknown argument: $1"; usage; exit 1 ;;
    esac
done

if [[ -f "$ENV_FILE" ]]; then
    echo "[live] Loading env file: $ENV_FILE"
    # shellcheck disable=SC1090
    source "$ENV_FILE"
fi

if [[ ! -x "$ENGINE_BIN" ]]; then
    echo "[live] ERROR: missing trading_engine binary at $ENGINE_BIN"
    echo "[live] Build first: mkdir -p build && cd build && cmake .. && make -j\$(nproc)"
    exit 1
fi

normalize_csv_upper() {
    local raw="$1"
    echo "$raw" | tr -d '[:space:]' | tr '[:lower:]' '[:upper:]'
}

require_live_creds() {
    local venue="$1"
    local key_var="${venue}_API_KEY"
    local secret_var="${venue}_API_SECRET"
    local live_key_var="LIVE_${venue}_API_KEY"
    local live_secret_var="LIVE_${venue}_API_SECRET"

    local key_val="${!live_key_var:-${!key_var:-}}"
    local secret_val="${!live_secret_var:-${!secret_var:-}}"

    if [[ -z "$key_val" || -z "$secret_val" ]]; then
        echo "[live] ERROR: missing credentials for $venue"
        echo "[live] Set $live_key_var/$live_secret_var (or $key_var/$secret_var)"
        exit 1
    fi

    export "$key_var=$key_val"
    export "$secret_var=$secret_val"
}

VENUES="$(normalize_csv_upper "$VENUES")"

IFS=',' read -r -a VENUE_LIST <<< "$VENUES"
for venue in "${VENUE_LIST[@]}"; do
    require_live_creds "$venue"
done

exec "$ENGINE_BIN" --mode live --venues "$VENUES" --symbol "$SYMBOL" --loop-interval-ms "$LOOP_INTERVAL_MS" "${EXTRA_ARGS[@]}"
