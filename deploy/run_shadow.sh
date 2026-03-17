#!/usr/bin/env bash
# Shadow trading runner — local four-venue validation (alpha publisher + C++ engine).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENGINE_BIN="${ENGINE_BIN:-$REPO_ROOT/build/bin/trading_engine}"
ENV_FILE_DEFAULT="$REPO_ROOT/config/shadow/trading.env"
CONFIG_FILE_DEFAULT="$REPO_ROOT/config/shadow/runtime.yaml"
PYTHON_BIN="${PYTHON_BIN:-}"

timestamp() {
    date -u +"%Y-%m-%dT%H:%M:%SZ"
}

log_info() {
    echo "[$(timestamp)] [shadow] [INFO] $*"
}

log_warn() {
    echo "[$(timestamp)] [shadow] [WARN] $*"
}

log_error() {
    echo "[$(timestamp)] [shadow] [ERROR] $*" >&2
}

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

    local symbol_from_cfg venues_from_cfg duration_from_cfg interval_from_cfg
    local env_from_cfg signal_file_from_cfg lob_feed_path_from_cfg
    local model_path_from_cfg secondary_model_from_cfg
    local train_ticks_from_cfg train_epochs_from_cfg report_interval_from_cfg
    local alpha_seq_len_from_cfg alpha_log_path_from_cfg
    local fallback_key_from_cfg fallback_secret_from_cfg

    symbol_from_cfg="$(read_yaml_value "$CONFIG_FILE" "symbol")"
    venues_from_cfg="$(read_yaml_value "$CONFIG_FILE" "venues")"
    duration_from_cfg="$(read_yaml_value "$CONFIG_FILE" "duration_secs")"
    interval_from_cfg="$(read_yaml_value "$CONFIG_FILE" "interval_ms")"
    env_from_cfg="$(read_yaml_value "$CONFIG_FILE" "env_file")"
    signal_file_from_cfg="$(read_yaml_value "$CONFIG_FILE" "signal_file")"
    lob_feed_path_from_cfg="$(read_yaml_value "$CONFIG_FILE" "lob_feed_path")"
    model_path_from_cfg="$(read_yaml_value "$CONFIG_FILE" "model_path")"
    secondary_model_from_cfg="$(read_yaml_value "$CONFIG_FILE" "secondary_model_path")"
    train_ticks_from_cfg="$(read_yaml_value "$CONFIG_FILE" "train_ticks")"
    train_epochs_from_cfg="$(read_yaml_value "$CONFIG_FILE" "train_epochs")"
    report_interval_from_cfg="$(read_yaml_value "$CONFIG_FILE" "report_interval")"
    alpha_seq_len_from_cfg="$(read_yaml_value "$CONFIG_FILE" "alpha_seq_len")"
    alpha_log_path_from_cfg="$(read_yaml_value "$CONFIG_FILE" "alpha_log_path")"
    fallback_key_from_cfg="$(read_yaml_value "$CONFIG_FILE" "shadow_fallback_api_key")"
    fallback_secret_from_cfg="$(read_yaml_value "$CONFIG_FILE" "shadow_fallback_api_secret")"

    [[ -n "$symbol_from_cfg" ]] && SYMBOL="$symbol_from_cfg"
    [[ -n "$venues_from_cfg" ]] && VENUES="$venues_from_cfg"
    [[ -n "$duration_from_cfg" ]] && DURATION_SECS="$duration_from_cfg"
    [[ -n "$interval_from_cfg" ]] && INTERVAL_MS="$interval_from_cfg"
    [[ -n "$env_from_cfg" ]] && ENV_FILE="$(resolve_config_path "$env_from_cfg")"
    [[ -n "$signal_file_from_cfg" ]] && SIGNAL_FILE="$signal_file_from_cfg"
    [[ -n "$lob_feed_path_from_cfg" ]] && LOB_FEED_PATH="$lob_feed_path_from_cfg"
    [[ -n "$model_path_from_cfg" ]] && MODEL_PATH="$(resolve_config_path "$model_path_from_cfg")"
    [[ -n "$secondary_model_from_cfg" ]] && SECONDARY_MODEL_PATH="$(resolve_config_path "$secondary_model_from_cfg")"
    [[ -n "$train_ticks_from_cfg" ]] && TRAIN_TICKS="$train_ticks_from_cfg"
    [[ -n "$train_epochs_from_cfg" ]] && TRAIN_EPOCHS="$train_epochs_from_cfg"
    [[ -n "$report_interval_from_cfg" ]] && REPORT_INTERVAL="$report_interval_from_cfg"
    [[ -n "$alpha_seq_len_from_cfg" ]] && ALPHA_SEQ_LEN="$alpha_seq_len_from_cfg"
    [[ -n "$alpha_log_path_from_cfg" ]] && ALPHA_LOG_PATH="$(resolve_config_path "$alpha_log_path_from_cfg")"
    [[ -n "$fallback_key_from_cfg" ]] && SHADOW_FALLBACK_API_KEY="$fallback_key_from_cfg"
    [[ -n "$fallback_secret_from_cfg" ]] && SHADOW_FALLBACK_API_SECRET="$fallback_secret_from_cfg"
}

usage() {
    cat <<USAGE
Usage: ./deploy/run_shadow.sh [options] [-- <extra trading_engine args>]

Options:
  --config <PATH>               YAML runtime defaults (default: config/shadow/runtime.yaml if present)
  --symbol <SYMBOL>             Trading symbol (default: BTCUSDT)
  --venues <CSV>                Venues CSV (default: BINANCE,KRAKEN,OKX,COINBASE)
  --duration <SECONDS>          Total runtime (default: 900)
  --interval-ms <MS>            Engine + Python loop interval (default: 1000)
  --env-file <PATH>             Optional env file (default: config/shadow/trading.env if present)
  --signal-file <PATH>          Alpha signal mmap (Python→C++ bridge)
                                (default: /ipc/neural_alpha_signal.bin)
  --lob-feed-path <PATH>        LOB ring-buffer mmap (C++→Python bridge)
                                (default: /ipc/trt_lob_feed.bin)
  --model-path <PATH>           Primary model checkpoint path
                                (default: models/neural_alpha_latest.pt)
  --secondary-model-path <PATH> Secondary model checkpoint path
                                (default: models/neural_alpha_secondary.pt)
  --train-ticks <N>             Train if model missing (default: 400)
  --train-epochs <N>            Train epochs when bootstrap training (default: 5)
  --report-interval <SEC>       Python shadow summary cadence (default: 60)
  --skip-alpha                  Do not launch Python shadow session
  --once                        Run one engine loop window and exit
  -h, --help                    Show this help

Credential resolution per venue (first non-empty wins):
  SHADOW_<VENUE>_API_KEY, <VENUE>_API_KEY, fallback=local-shadow-key
  SHADOW_<VENUE>_API_SECRET, <VENUE>_API_SECRET, fallback=local-shadow-secret
USAGE
}

SYMBOL="BTCUSDT"
VENUES="BINANCE,KRAKEN,OKX,COINBASE"
DURATION_SECS=900
INTERVAL_MS=1000
RUN_ONCE=0
ENV_FILE="$ENV_FILE_DEFAULT"
SIGNAL_FILE="/ipc/neural_alpha_signal.bin"
LOB_FEED_PATH="/ipc/trt_lob_feed.bin"
MODEL_PATH="$REPO_ROOT/models/neural_alpha_latest.pt"
SECONDARY_MODEL_PATH="$REPO_ROOT/models/neural_alpha_secondary.pt"
MODEL_PATH_SET=0
SECONDARY_MODEL_PATH_SET=0
TRAIN_TICKS=400
TRAIN_EPOCHS=5
REPORT_INTERVAL=60
ALPHA_SEQ_LEN=64
ALPHA_LOG_PATH="$REPO_ROOT/logs/neural_alpha_shadow.jsonl"
SHADOW_FALLBACK_API_KEY="local-shadow-key"
SHADOW_FALLBACK_API_SECRET="local-shadow-secret"
SKIP_ALPHA=0
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
        --duration) DURATION_SECS="$2"; shift 2 ;;
        --interval-ms) INTERVAL_MS="$2"; shift 2 ;;
        --env-file) ENV_FILE="$2"; shift 2 ;;
        --signal-file) SIGNAL_FILE="$2"; shift 2 ;;
        --lob-feed-path) LOB_FEED_PATH="$2"; shift 2 ;;
        --model-path) MODEL_PATH="$2"; MODEL_PATH_SET=1; shift 2 ;;
        --secondary-model-path) SECONDARY_MODEL_PATH="$2"; SECONDARY_MODEL_PATH_SET=1; shift 2 ;;
        --train-ticks) TRAIN_TICKS="$2"; shift 2 ;;
        --train-epochs) TRAIN_EPOCHS="$2"; shift 2 ;;
        --report-interval) REPORT_INTERVAL="$2"; shift 2 ;;
        --skip-alpha) SKIP_ALPHA=1; shift ;;
        --once) RUN_ONCE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        --) shift; EXTRA_ARGS=("$@"); break ;;
        *) log_error "Unknown argument: $1"; usage; exit 1 ;;
    esac
done

SYMBOL_TAG="$(echo "$SYMBOL" | tr '[:upper:]' '[:lower:]')"
if [[ "$MODEL_PATH_SET" -eq 0 && "$MODEL_PATH" == "$REPO_ROOT/models/neural_alpha_latest.pt" ]]; then
    MODEL_PATH="$REPO_ROOT/models/neural_alpha_${SYMBOL_TAG}_latest.pt"
fi
if [[ "$SECONDARY_MODEL_PATH_SET" -eq 0 && "$SECONDARY_MODEL_PATH" == "$REPO_ROOT/models/neural_alpha_secondary.pt" ]]; then
    SECONDARY_MODEL_PATH="$REPO_ROOT/models/neural_alpha_${SYMBOL_TAG}_secondary.pt"
fi

if [[ -f "$ENV_FILE" ]]; then
    log_info "Loading env file: $ENV_FILE"
    # shellcheck disable=SC1090
    source "$ENV_FILE"
fi

if [[ ! -x "$ENGINE_BIN" ]]; then
    log_error "Missing trading_engine binary at $ENGINE_BIN"
    log_info "Build first: mkdir -p build && cd build && cmake .. && make -j\$(nproc)"
    exit 1
fi

set_shadow_creds() {
    local venue="$1"
    local key_var="${venue}_API_KEY"
    local secret_var="${venue}_API_SECRET"
    local shadow_key_var="SHADOW_${venue}_API_KEY"
    local shadow_secret_var="SHADOW_${venue}_API_SECRET"

    local key_val="${!shadow_key_var:-${!key_var:-$SHADOW_FALLBACK_API_KEY}}"
    local secret_val="${!shadow_secret_var:-${!secret_var:-$SHADOW_FALLBACK_API_SECRET}}"

    export "$key_var=$key_val"
    export "$secret_var=$secret_val"
}

normalize_csv_upper() {
    local raw="$1"
    # Strip whitespace and normalize to uppercase for venue/env variable resolution.
    echo "$raw" | tr -d '[:space:]' | tr '[:lower:]' '[:upper:]'
}

choose_python() {
    if [[ -n "$PYTHON_BIN" ]]; then
        return 0
    fi
    for candidate in python3.12 python3.11 python3.10 python3; do
        if command -v "$candidate" >/dev/null 2>&1; then
            if "$candidate" -c 'import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)' >/dev/null 2>&1; then
                PYTHON_BIN="$candidate"
                return 0
            fi
        fi
    done
    log_error "Python 3.10+ is required for neural alpha shadow session."
    return 1
}

print_shadow_summary() {
    if [[ "$SKIP_ALPHA" -eq 1 ]]; then
        log_info "Skipped alpha session; no Python shadow statistics available."
        return
    fi

    if [[ ! -f "$ALPHA_LOG_PATH" ]]; then
        log_warn "Alpha log path not found: $ALPHA_LOG_PATH"
        return
    fi

    if ! choose_python; then
        log_warn "Cannot summarize shadow session without a valid Python 3.10+ binary."
        return
    fi

    log_info "Final shadow session statistics from $ALPHA_LOG_PATH"
    "$PYTHON_BIN" - "$ALPHA_LOG_PATH" <<'PY'
import json
import math
import statistics
import sys

path = sys.argv[1]
signals: list[float] = []
mid_prices: list[float] = []
safe_mode_events = 0

with open(path, "r", encoding="utf-8") as fp:
    for line in fp:
        line = line.strip()
        if not line:
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        signal = event.get("signal")
        mid = event.get("mid_price")
        if isinstance(signal, (int, float)):
            signals.append(float(signal))
        if isinstance(mid, (int, float)):
            mid_prices.append(float(mid))
        if event.get("safe_mode"):
            safe_mode_events += 1

if not signals:
    print("  [summary] no valid signal records found")
    raise SystemExit(0)

mean_sig = statistics.fmean(signals) * 1e4
std_sig = statistics.pstdev(signals) * 1e4 if len(signals) > 1 else 0.0
max_abs_sig = max(abs(s) for s in signals) * 1e4

returns: list[float] = []
for prev, cur in zip(mid_prices, mid_prices[1:]):
    if prev > 0:
        returns.append((cur - prev) / prev)

ic = 0.0
if returns and len(signals) > 1:
    aligned = min(len(returns), len(signals) - 1)
    x = signals[:aligned]
    y = returns[:aligned]
    mx = statistics.fmean(x)
    my = statistics.fmean(y)
    cov = sum((a - mx) * (b - my) for a, b in zip(x, y)) / aligned
    vx = sum((a - mx) ** 2 for a in x) / aligned
    vy = sum((b - my) ** 2 for b in y) / aligned
    if vx > 0 and vy > 0:
        ic = cov / math.sqrt(vx * vy)

print(f"  [summary] signal_count={len(signals)}")
print(f"  [summary] signal_mean_bps={mean_sig:.3f}")
print(f"  [summary] signal_std_bps={std_sig:.3f}")
print(f"  [summary] max_abs_signal_bps={max_abs_sig:.3f}")
print(f"  [summary] observed_return_count={len(returns)}")
print(f"  [summary] realised_ic={ic:.4f}")
print(f"  [summary] safe_mode_events={safe_mode_events}")
PY
}

VENUES="$(normalize_csv_upper "$VENUES")"
ALPHA_EXCHANGES="$VENUES"

IFS=',' read -r -a VENUE_LIST <<< "$VENUES"
for venue in "${VENUE_LIST[@]}"; do
    set_shadow_creds "$venue"
done

engine_args=(--mode shadow --venues "$VENUES" --symbol "$SYMBOL" --loop-interval-ms "$INTERVAL_MS")
if [[ ${#EXTRA_ARGS[@]} -gt 0 ]]; then
    engine_args+=("${EXTRA_ARGS[@]}")
fi

PY_PID=""
cleanup() {
    if [[ -n "$PY_PID" ]]; then
        kill "$PY_PID" 2>/dev/null || true
        wait "$PY_PID" 2>/dev/null || true
    fi
    print_shadow_summary
}
trap cleanup EXIT INT TERM

# Ensure IPC directory exists before either process starts.  The C++ engine
# writes the LOB ring buffer here even when --skip-alpha is set.
mkdir -p "$(dirname "$LOB_FEED_PATH")"

if [[ "$SKIP_ALPHA" -eq 0 ]]; then
    choose_python
    mkdir -p "$REPO_ROOT/logs" "$REPO_ROOT/models"
    ALPHA_ARGS=(
        -m research.neural_alpha.shadow_session
        --signal-file "$SIGNAL_FILE"
        --lob-feed-path "$LOB_FEED_PATH"
        --duration "$DURATION_SECS"
        --interval-ms "$INTERVAL_MS"
        --report-interval "$REPORT_INTERVAL"
        --seq-len "$ALPHA_SEQ_LEN"
        --symbol "$SYMBOL"
        --exchanges "$ALPHA_EXCHANGES"
        --model-path "$MODEL_PATH"
        --secondary-model-path "$SECONDARY_MODEL_PATH"
        --train-ticks "$TRAIN_TICKS"
        --train-epochs "$TRAIN_EPOCHS"
        --log-path "$ALPHA_LOG_PATH"
    )

    log_info "Starting python shadow session with $PYTHON_BIN (symbol=$SYMBOL, exchanges=$ALPHA_EXCHANGES, signals -> $SIGNAL_FILE, log=$ALPHA_LOG_PATH)"
    (cd "$REPO_ROOT" && "$PYTHON_BIN" "${ALPHA_ARGS[@]}") &
    PY_PID=$!
fi

log_info "Starting C++ shadow runner symbol=$SYMBOL venues=$VENUES duration=${DURATION_SECS}s interval=${INTERVAL_MS}ms"

if [[ "$RUN_ONCE" -eq 1 ]]; then
    DURATION_SECS="$(awk "BEGIN { printf \"%.3f\", ($INTERVAL_MS * 2)/1000 }")"
fi

if timeout "$DURATION_SECS" "$ENGINE_BIN" "${engine_args[@]}"; then
    :
else
    timeout_status=$?
    if [[ "$timeout_status" -ne 124 ]]; then
        log_error "trading_engine exited with status $timeout_status"
        exit "$timeout_status"
    fi
fi

log_info "Completed shadow run window"
