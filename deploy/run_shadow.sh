#!/usr/bin/env bash
# Shadow trading runner — local four-venue validation (alpha publisher + C++ engine).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENGINE_BIN="${ENGINE_BIN:-$REPO_ROOT/build/bin/trading_engine}"
ENV_FILE_DEFAULT="$REPO_ROOT/config/shadow/trading.env"
CONFIG_FILE_DEFAULT="$REPO_ROOT/config/shadow/runtime.yaml"
PYTHON_BIN="${PYTHON_BIN:-}"
RUNTIME_MODE_DIR_NAME=""
RUNTIME_MODE_DIR_PATH=""

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

read_yaml_map_entries() {
    local file_path="$1"
    local map_key="$2"
    awk -v map_key="$map_key" '
        BEGIN { in_map = 0 }
        $0 ~ "^[[:space:]]*" map_key ":[[:space:]]*$" { in_map = 1; next }
        in_map == 1 && $0 ~ "^[^[:space:]]" { in_map = 0 }
        in_map == 1 && $0 ~ "^[[:space:]]+[A-Za-z0-9_]+:[[:space:]]*" {
            line = $0
            gsub(/^[[:space:]]+/, "", line)
            split(line, parts, ":")
            key = parts[1]
            sub(/^[^:]*:[[:space:]]*/, "", line)
            sub(/[[:space:]]+#.*/, "", line)
            gsub(/^["\047]|["\047]$/, "", line)
            print key "=" line
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
    local model_path_from_cfg secondary_model_from_cfg regime_model_path_from_cfg
    local train_ticks_from_cfg train_epochs_from_cfg report_interval_from_cfg
    local alpha_seq_len_from_cfg alpha_log_path_from_cfg
    local continuous_train_every_ticks_from_cfg continuous_train_window_ticks_from_cfg
    local strategy_mode_from_cfg futures_config_from_cfg

    local safe_mode_ticks_from_cfg drift_min_samples_from_cfg drift_ic_floor_from_cfg

    symbol_from_cfg="$(read_yaml_value "$CONFIG_FILE" "symbol")"
    venues_from_cfg="$(read_yaml_value "$CONFIG_FILE" "venues")"
    duration_from_cfg="$(read_yaml_value "$CONFIG_FILE" "duration_secs")"
    interval_from_cfg="$(read_yaml_value "$CONFIG_FILE" "interval_ms")"
    env_from_cfg="$(read_yaml_value "$CONFIG_FILE" "env_file")"
    signal_file_from_cfg="$(read_yaml_value "$CONFIG_FILE" "signal_file")"
    lob_feed_path_from_cfg="$(read_yaml_value "$CONFIG_FILE" "lob_feed_path")"
    model_path_from_cfg="$(read_yaml_value "$CONFIG_FILE" "model_path")"
    secondary_model_from_cfg="$(read_yaml_value "$CONFIG_FILE" "secondary_model_path")"
    regime_model_path_from_cfg="$(read_yaml_value "$CONFIG_FILE" "regime_model_path")"
    train_ticks_from_cfg="$(read_yaml_value "$CONFIG_FILE" "train_ticks")"
    train_epochs_from_cfg="$(read_yaml_value "$CONFIG_FILE" "train_epochs")"
    report_interval_from_cfg="$(read_yaml_value "$CONFIG_FILE" "report_interval")"
    alpha_seq_len_from_cfg="$(read_yaml_value "$CONFIG_FILE" "alpha_seq_len")"
    alpha_log_path_from_cfg="$(read_yaml_value "$CONFIG_FILE" "alpha_log_path")"
    continuous_train_every_ticks_from_cfg="$(read_yaml_value "$CONFIG_FILE" "continuous_train_every_ticks")"
    continuous_train_window_ticks_from_cfg="$(read_yaml_value "$CONFIG_FILE" "continuous_train_window_ticks")"
    strategy_mode_from_cfg="$(read_yaml_value "$CONFIG_FILE" "strategy_mode")"
    futures_config_from_cfg="$(read_yaml_value "$CONFIG_FILE" "futures_config")"

    safe_mode_ticks_from_cfg="$(read_yaml_value "$CONFIG_FILE" "safe_mode_ticks")"
    drift_min_samples_from_cfg="$(read_yaml_value "$CONFIG_FILE" "drift_min_samples")"
    drift_ic_floor_from_cfg="$(read_yaml_value "$CONFIG_FILE" "drift_ic_floor")"

    [[ -n "$symbol_from_cfg" ]] && SYMBOL="$symbol_from_cfg"
    [[ -n "$venues_from_cfg" ]] && VENUES="$venues_from_cfg"
    [[ -n "$duration_from_cfg" ]] && DURATION_SECS="$duration_from_cfg"
    [[ -n "$interval_from_cfg" ]] && INTERVAL_MS="$interval_from_cfg"
    [[ -n "$env_from_cfg" ]] && ENV_FILE="$(resolve_config_path "$env_from_cfg")"
    [[ -n "$signal_file_from_cfg" ]] && SIGNAL_FILE="$signal_file_from_cfg"
    [[ -n "$lob_feed_path_from_cfg" ]] && LOB_FEED_PATH="$lob_feed_path_from_cfg"
    [[ -n "$model_path_from_cfg" ]] && MODEL_PATH="$(resolve_config_path "$model_path_from_cfg")"
    [[ -n "$secondary_model_from_cfg" ]] && SECONDARY_MODEL_PATH="$(resolve_config_path "$secondary_model_from_cfg")"
    [[ -n "$regime_model_path_from_cfg" ]] && REGIME_MODEL_PATH="$(resolve_config_path "$regime_model_path_from_cfg")"
    [[ -n "$train_ticks_from_cfg" ]] && TRAIN_TICKS="$train_ticks_from_cfg"
    [[ -n "$train_epochs_from_cfg" ]] && TRAIN_EPOCHS="$train_epochs_from_cfg"
    [[ -n "$report_interval_from_cfg" ]] && REPORT_INTERVAL="$report_interval_from_cfg"
    [[ -n "$alpha_seq_len_from_cfg" ]] && ALPHA_SEQ_LEN="$alpha_seq_len_from_cfg"
    [[ -n "$alpha_log_path_from_cfg" ]] && ALPHA_LOG_PATH="$(resolve_config_path "$alpha_log_path_from_cfg")"
    [[ -n "$continuous_train_every_ticks_from_cfg" ]] && CONTINUOUS_TRAIN_EVERY_TICKS="$continuous_train_every_ticks_from_cfg"
    [[ -n "$continuous_train_window_ticks_from_cfg" ]] && CONTINUOUS_TRAIN_WINDOW_TICKS="$continuous_train_window_ticks_from_cfg"
    [[ -n "$strategy_mode_from_cfg" ]] && STRATEGY_MODE="$strategy_mode_from_cfg"
    [[ -n "$futures_config_from_cfg" ]] && FUTURES_CONFIG="$(resolve_config_path "$futures_config_from_cfg")"

    [[ -n "$safe_mode_ticks_from_cfg" ]] && SAFE_MODE_TICKS="$safe_mode_ticks_from_cfg"
    [[ -n "$drift_min_samples_from_cfg" ]] && DRIFT_MIN_SAMPLES="$drift_min_samples_from_cfg"
    [[ -n "$drift_ic_floor_from_cfg" ]] && DRIFT_IC_FLOOR="$drift_ic_floor_from_cfg"
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
                                (default: /tmp/trt_ipc/neural_alpha_signal.bin)
  --lob-feed-path <PATH>        LOB ring-buffer mmap (C++→Python bridge)
                                (default: /tmp/trt_ipc/trt_lob_feed.bin)
  --model-path <PATH>           Primary model checkpoint path
                                (default: models/neural_alpha_<symbol>_latest.pt)
  --secondary-model-path <PATH> Secondary model checkpoint path
                                (default: models/neural_alpha_<symbol>_secondary.pt)
  --regime-model-path <PATH>    Regime model artifact path
                                (default: models/r2_regime_model_<symbol>.json)
  --train-ticks <N>             Bootstrap-train if any model is missing (default: 1000)
  --train-epochs <N>            Epochs for bootstrap and continuous training (default: 10)
  --report-interval <SEC>       Python shadow summary cadence (default: 60)
  --continuous-train-every-ticks <N>  Ticks between incremental retrains (default: 400)
  --continuous-train-window-ticks <N> Tick window used for each retrain (default: 1000)
  --strategy-mode <MODE>        Runtime strategy mode: spot_only or futures_only
  --futures-config <PATH>       Futures runtime YAML path (default: config/shadow/futures.yaml)
  --safe-mode-ticks <N>         Ticks of zeroed signal after a drift/canary event (default: 30)
  --drift-min-samples <N>       Minimum outcomes before DriftGuard can fire (default: 100)
  --drift-ic-floor <FLOAT>      IC floor below which DriftGuard fires (default: -0.08)
  --skip-alpha                  Do not launch Python shadow session
  --once                        Run one engine loop window and exit
  -h, --help                    Show this help

Credential resolution per venue (first non-empty wins):
  SHADOW_<VENUE>_API_KEY, <VENUE>_API_KEY
  SHADOW_<VENUE>_API_SECRET, <VENUE>_API_SECRET

  Coinbase Advanced Trade requires a valid EC private-key PEM for the level2
  WebSocket channel.  Set COINBASE_API_KEY and COINBASE_API_SECRET (or the
  SHADOW_ prefixed variants) to real credentials; without them the C++ feed
  handler will fast-fail after one attempt.  The Python REST fallback for
  training data (_fetch_coinbase_l5) uses the public product_book endpoint
  and works without credentials.
USAGE
}

RUN_ONCE=0
MODEL_PATH_SET=0
SECONDARY_MODEL_PATH_SET=0
SKIP_ALPHA=0
CONFIG_FILE="$CONFIG_FILE_DEFAULT"
STRATEGY_MODE="${STRATEGY_MODE:-spot_only}"
FUTURES_CONFIG="${FUTURES_CONFIG:-}"
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
        --regime-model-path) REGIME_MODEL_PATH="$2"; shift 2 ;;
        --train-ticks) TRAIN_TICKS="$2"; shift 2 ;;
        --train-epochs) TRAIN_EPOCHS="$2"; shift 2 ;;
        --report-interval) REPORT_INTERVAL="$2"; shift 2 ;;
        --continuous-train-every-ticks) CONTINUOUS_TRAIN_EVERY_TICKS="$2"; shift 2 ;;
        --continuous-train-window-ticks) CONTINUOUS_TRAIN_WINDOW_TICKS="$2"; shift 2 ;;
        --strategy-mode) STRATEGY_MODE="$2"; shift 2 ;;
        --futures-config) FUTURES_CONFIG="$(resolve_config_path "$2")"; shift 2 ;;
        --safe-mode-ticks) SAFE_MODE_TICKS="$2"; shift 2 ;;
        --drift-min-samples) DRIFT_MIN_SAMPLES="$2"; shift 2 ;;
        --drift-ic-floor) DRIFT_IC_FLOOR="$2"; shift 2 ;;
        --skip-alpha) SKIP_ALPHA=1; shift ;;
        --once) RUN_ONCE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        --) shift; EXTRA_ARGS=("$@"); break ;;
        *) log_error "Unknown argument: $1"; usage; exit 1 ;;
    esac
done

if [[ -z "$FUTURES_CONFIG" ]]; then
    FUTURES_CONFIG="$REPO_ROOT/config/shadow/futures.yaml"
fi

if [[ "$STRATEGY_MODE" != "spot_only" && "$STRATEGY_MODE" != "futures_only" ]]; then
    log_error "Invalid strategy_mode=$STRATEGY_MODE (expected spot_only or futures_only)"
    exit 1
fi

SYMBOL_TAG="$(echo "$SYMBOL" | tr '[:upper:]' '[:lower:]')"
if [[ "$MODEL_PATH_SET" -eq 0 ]]; then
    if [[ -z "$MODEL_PATH" || "$MODEL_PATH" == */neural_alpha_latest.pt ]]; then
        MODEL_PATH="$REPO_ROOT/models/neural_alpha_${SYMBOL_TAG}_latest.pt"
    fi
fi
if [[ "$SECONDARY_MODEL_PATH_SET" -eq 0 ]]; then
    if [[ -z "$SECONDARY_MODEL_PATH" || "$SECONDARY_MODEL_PATH" == */neural_alpha_secondary.pt ]]; then
        SECONDARY_MODEL_PATH="$REPO_ROOT/models/neural_alpha_${SYMBOL_TAG}_secondary.pt"
    fi
fi
if [[ -z "$REGIME_MODEL_PATH" || "$REGIME_MODEL_PATH" == */r2_regime_model.json ]]; then
    REGIME_MODEL_PATH="$REPO_ROOT/models/r2_regime_model_${SYMBOL_TAG}.json"
fi

if [[ -f "$ENV_FILE" ]]; then
    log_info "Loading env file: $ENV_FILE"
    # shellcheck disable=SC1090
    source "$ENV_FILE"
fi

set_shadow_creds() {
    local venue="$1"
    local key_var="${venue}_API_KEY"
    local secret_var="${venue}_API_SECRET"
    local shadow_key_var="SHADOW_${venue}_API_KEY"
    local shadow_secret_var="SHADOW_${venue}_API_SECRET"

    local key_val="${!shadow_key_var:-${!key_var:-}}"
    local secret_val="${!shadow_secret_var:-${!secret_var:-}}"

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
effective_signals_bps: list[float] = []
mid_prices: list[float] = []
risk_scores: list[float] = []
safe_mode_events = 0
gated_events = 0
elapsed_samples: list[int] = []
p_calm_vals: list[float] = []
p_trending_vals: list[float] = []
p_shock_vals: list[float] = []
p_illiquid_vals: list[float] = []

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
        effective_signal_bps = event.get("ret_mid_bps")
        mid = event.get("mid_price")
        risk = event.get("risk_score")
        elapsed_ns = event.get("session_elapsed_ns")
        if isinstance(signal, (int, float)):
            signals.append(float(signal))
        if isinstance(effective_signal_bps, (int, float)):
            effective_signals_bps.append(float(effective_signal_bps))
        if isinstance(mid, (int, float)):
            mid_prices.append(float(mid))
        if isinstance(risk, (int, float)):
            risk_scores.append(float(risk))
        if isinstance(elapsed_ns, int):
            elapsed_samples.append(elapsed_ns)
        if event.get("safe_mode"):
            safe_mode_events += 1
        if event.get("gated"):
            gated_events += 1
        for k, lst in (("p_calm", p_calm_vals), ("p_trending", p_trending_vals),
                       ("p_shock", p_shock_vals), ("p_illiquid", p_illiquid_vals)):
            v = event.get(k)
            if isinstance(v, (int, float)):
                lst.append(float(v))

if not signals:
    print("  [summary] no valid signal records found")
    raise SystemExit(0)

n = len(signals)
mean_sig = statistics.fmean(effective_signals_bps) if effective_signals_bps else 0.0
std_sig = statistics.pstdev(effective_signals_bps) if len(effective_signals_bps) > 1 else 0.0
max_abs_sig = max((abs(s) for s in effective_signals_bps), default=0.0)
raw_mean_sig = statistics.fmean(signals) * 1e4
raw_std_sig = statistics.pstdev(signals) * 1e4 if n > 1 else 0.0
safe_pct = 100.0 * safe_mode_events / n if n else 0.0
gated_pct = 100.0 * gated_events / n if n else 0.0
avg_risk = statistics.fmean(risk_scores) if risk_scores else 0.0
duration_min = (elapsed_samples[-1] - elapsed_samples[0]) / 1e9 / 60.0 if len(elapsed_samples) > 1 else 0.0

returns: list[float] = []
for prev, cur in zip(mid_prices, mid_prices[1:]):
    if prev > 0:
        returns.append((cur - prev) / prev)

ic = 0.0
if returns and n > 1:
    aligned = min(len(returns), n - 1)
    x = signals[:aligned]
    y = returns[:aligned]
    mx = statistics.fmean(x)
    my = statistics.fmean(y)
    cov = sum((a - mx) * (b - my) for a, b in zip(x, y)) / aligned
    vx = sum((a - mx) ** 2 for a in x) / aligned
    vy = sum((b - my) ** 2 for b in y) / aligned
    if vx > 0 and vy > 0:
        ic = cov / math.sqrt(vx * vy)

icir = 0.0
aligned_n = min(len(returns), n - 1)
if aligned_n >= 40:
    chunk = aligned_n // 4
    ic_chunks: list[float] = []
    for i in range(4):
        xs = signals[i * chunk:(i + 1) * chunk]
        ys = returns[i * chunk:(i + 1) * chunk]
        mx2, my2 = statistics.fmean(xs), statistics.fmean(ys)
        cov2 = sum((a - mx2) * (b - my2) for a, b in zip(xs, ys)) / chunk
        vx2 = sum((a - mx2) ** 2 for a in xs) / chunk
        vy2 = sum((b - my2) ** 2 for b in ys) / chunk
        if vx2 > 1e-18 and vy2 > 1e-18:
            ic_chunks.append(cov2 / math.sqrt(vx2 * vy2))
    if len(ic_chunks) >= 2:
        ic_mean = statistics.fmean(ic_chunks)
        ic_std = statistics.pstdev(ic_chunks)
        if ic_std > 1e-9:
            icir = ic_mean / ic_std * math.sqrt(252)

SEP = "=" * 60
print(SEP)
print("  Shadow Session Report")
print(SEP)
print()
print("── Signal statistics ────────────────────────────────────────")
print(f"  Signals generated   : {n}")
print(f"  Session duration    : {duration_min:.1f} min")
print(f"  Mean signal         : {mean_sig:.4f} bps")
print(f"  Signal std          : {std_sig:.4f} bps")
print(f"  Max |signal|        : {max_abs_sig:.4f} bps")
print(f"  Mean raw signal     : {raw_mean_sig:.4f} bps")
print(f"  Raw signal std      : {raw_std_sig:.4f} bps")
print(f"  Avg risk score      : {avg_risk:.4f}")
print()
print("── Alpha quality ────────────────────────────────────────────")
print(f"  Realised IC         : {ic:.4f}")
print(f"  ICIR (annualised)   : {icir:.4f}")
print(f"  Safe-mode events    : {safe_mode_events}  ({safe_pct:.1f}% of ticks)")
print(f"  Gated events        : {gated_events}  ({gated_pct:.1f}% of ticks)")
print()
if p_calm_vals:
    print("── Regime distribution (session avg) ────────────────────────")
    print(f"  p_calm              : {statistics.fmean(p_calm_vals):.3f}")
    print(f"  p_trending          : {statistics.fmean(p_trending_vals):.3f}")
    print(f"  p_shock             : {statistics.fmean(p_shock_vals):.3f}")
    print(f"  p_illiquid          : {statistics.fmean(p_illiquid_vals):.3f}")
    print()
print("── Readiness check ──────────────────────────────────────────")
print("  Run >= 2 weeks before promoting to live.")
print("  IC should be consistently positive (> 0.02).")
print("  Safe-mode rate should remain below 10%.")
print("  ICIR (annualised) target: > 0.5.")
print()
print(SEP)
PY

    local decisions_log="$REPO_ROOT/logs/shadow_decisions.jsonl"
    if [[ -f "$decisions_log" ]]; then
        log_info "Running full shadow metrics report..."
        (cd "$REPO_ROOT" && "$PYTHON_BIN" -m research.backtest.shadow_metrics \
            --signals "$ALPHA_LOG_PATH" \
            --decisions "$decisions_log" \
) || true
    fi
}

VENUES="$(normalize_csv_upper "$VENUES")"
ALPHA_EXCHANGES="$VENUES"

IFS=',' read -r -a VENUE_LIST <<< "$VENUES"
for venue in "${VENUE_LIST[@]}"; do
    set_shadow_creds "$venue"
done

if [[ " ${VENUE_LIST[*]} " == *" COINBASE "* ]]; then
    _cb_key="${COINBASE_API_KEY:-}"
    _cb_secret="${COINBASE_API_SECRET:-}"
    if [[ -z "$_cb_key" || -z "$_cb_secret" ]]; then
        log_warn "Coinbase credentials not configured (COINBASE_API_KEY / COINBASE_API_SECRET unset)."
        log_warn "The C++ Coinbase WebSocket feed will be skipped; training data uses REST fallback."
    fi
    unset _cb_key _cb_secret
fi

FUTURES_ENABLED="false"
FUTURES_STRATEGY_MODE=""
FUTURES_POSITION_MODE=""
FUTURES_DEFAULT_LEVERAGE=""
FUTURES_DEADBAND_SIGNAL_BPS=""
FUTURES_FLIP_ENABLED=""
FUTURES_FLIP_MIN_ABS_SIGNAL_BPS=""
FUTURES_LEVERAGE_CAP_ENTRIES=()

if [[ -f "$FUTURES_CONFIG" ]]; then
    FUTURES_ENABLED="$(read_yaml_value "$FUTURES_CONFIG" "enabled")"
    FUTURES_STRATEGY_MODE="$(read_yaml_value "$FUTURES_CONFIG" "strategy_mode")"
    FUTURES_POSITION_MODE="$(read_yaml_value "$FUTURES_CONFIG" "position_mode")"
    FUTURES_DEFAULT_LEVERAGE="$(read_yaml_value "$FUTURES_CONFIG" "default_leverage")"
    FUTURES_DEADBAND_SIGNAL_BPS="$(read_yaml_value "$FUTURES_CONFIG" "deadband_signal_bps")"
    FUTURES_FLIP_ENABLED="$(read_yaml_value "$FUTURES_CONFIG" "flip_enabled")"
    FUTURES_FLIP_MIN_ABS_SIGNAL_BPS="$(read_yaml_value "$FUTURES_CONFIG" "flip_min_abs_signal_bps")"
    while IFS= read -r entry; do
        [[ -n "$entry" ]] && FUTURES_LEVERAGE_CAP_ENTRIES+=("$entry")
    done < <(read_yaml_map_entries "$FUTURES_CONFIG" "leverage_caps")
fi

if [[ "$STRATEGY_MODE" == "futures_only" ]]; then
    if [[ ! -f "$FUTURES_CONFIG" ]]; then
        log_error "strategy_mode=futures_only requires futures config file: $FUTURES_CONFIG"
        exit 1
    fi
    if [[ "$FUTURES_ENABLED" != "true" ]]; then
        log_error "strategy_mode=futures_only requires futures enabled=true in $FUTURES_CONFIG"
        exit 1
    fi
    if [[ -z "$FUTURES_DEFAULT_LEVERAGE" ]]; then
        log_error "strategy_mode=futures_only requires default_leverage in $FUTURES_CONFIG"
        exit 1
    fi
    if [[ "$FUTURES_POSITION_MODE" != "one_way" && "$FUTURES_POSITION_MODE" != "hedge" ]]; then
        log_error "Invalid futures position_mode=$FUTURES_POSITION_MODE (expected one_way or hedge)"
        exit 1
    fi
    if [[ -n "$FUTURES_STRATEGY_MODE" && "$FUTURES_STRATEGY_MODE" != "$STRATEGY_MODE" ]]; then
        log_error "futures.yaml strategy_mode=$FUTURES_STRATEGY_MODE does not match runtime strategy_mode=$STRATEGY_MODE"
        exit 1
    fi
    if [[ -z "${BINANCE_API_KEY:-}" || -z "${BINANCE_API_SECRET:-}" ]]; then
        log_error "strategy_mode=futures_only requires BINANCE_API_KEY and BINANCE_API_SECRET credentials"
        exit 1
    fi
fi

RUNTIME_MODE_DIR_NAME="shadow_runtime_$$"
RUNTIME_MODE_DIR_PATH="$REPO_ROOT/config/$RUNTIME_MODE_DIR_NAME"
mkdir -p "$RUNTIME_MODE_DIR_PATH"
ln -s "$REPO_ROOT/config/shadow/risk.yaml" "$RUNTIME_MODE_DIR_PATH/risk.yaml"
ln -s "$REPO_ROOT/config/shadow/routing.yaml" "$RUNTIME_MODE_DIR_PATH/routing.yaml"
ln -s "$REPO_ROOT/config/shadow/venue_quality.yaml" "$RUNTIME_MODE_DIR_PATH/venue_quality.yaml"

ENGINE_BASE_FILE="$REPO_ROOT/config/shadow/engine.yaml"
ENGINE_RUNTIME_FILE="$RUNTIME_MODE_DIR_PATH/engine.yaml"
awk -v strategy_mode="$STRATEGY_MODE" -v futures_enabled="$FUTURES_ENABLED" \
    -v futures_hedge_mode="$([[ "$FUTURES_POSITION_MODE" == "hedge" ]] && echo "true" || echo "false")" \
    -v futures_default_leverage="$FUTURES_DEFAULT_LEVERAGE" '
    BEGIN {
        fs_enabled = (futures_enabled == "" ? "false" : futures_enabled);
        fs_hedge = (futures_hedge_mode == "" ? "false" : futures_hedge_mode);
    }
    /^strategy_mode:/ { print "strategy_mode: " strategy_mode; next }
    /^binance_futures_enabled:/ { print "binance_futures_enabled: " fs_enabled; next }
    /^binance_futures_hedge_mode:/ { print "binance_futures_hedge_mode: " fs_hedge; next }
    /^binance_futures_default_leverage_cap:/ {
        if (futures_default_leverage != "") {
            print "binance_futures_default_leverage_cap: " futures_default_leverage;
            next
        }
    }
    /^binance_futures_leverage_cap_/ { next }
    { print }
' "$ENGINE_BASE_FILE" > "$ENGINE_RUNTIME_FILE"

if [[ "${#FUTURES_LEVERAGE_CAP_ENTRIES[@]}" -gt 0 ]]; then
    for entry in "${FUTURES_LEVERAGE_CAP_ENTRIES[@]}"; do
        cap_symbol="${entry%%=*}"
        cap_value="${entry#*=}"
        if [[ -n "$cap_symbol" && -n "$cap_value" ]]; then
            echo "binance_futures_leverage_cap_${cap_symbol}: ${cap_value}" >> "$ENGINE_RUNTIME_FILE"
        fi
    done
fi

if [[ -n "$FUTURES_DEADBAND_SIGNAL_BPS" ]]; then
    PORTFOLIO_BASE_FILE="$REPO_ROOT/config/shadow/portfolio.yaml"
    PORTFOLIO_RUNTIME_FILE="$RUNTIME_MODE_DIR_PATH/portfolio.yaml"
    awk -v deadband_signal_bps="$FUTURES_DEADBAND_SIGNAL_BPS" -v strategy_mode="$STRATEGY_MODE" '
        /^deadband_signal_bps:/ { print "deadband_signal_bps: " deadband_signal_bps; next }
        /^long_only:/ {
            if (strategy_mode == "futures_only") {
                print "long_only: false";
                next
            }
        }
        { print }
    ' "$PORTFOLIO_BASE_FILE" > "$PORTFOLIO_RUNTIME_FILE"
else
    ln -s "$REPO_ROOT/config/shadow/portfolio.yaml" "$RUNTIME_MODE_DIR_PATH/portfolio.yaml"
fi

engine_args=(--mode "$RUNTIME_MODE_DIR_NAME" --venues "$VENUES" --symbol "$SYMBOL" --loop-interval-ms "$INTERVAL_MS")
if [[ ${#EXTRA_ARGS[@]} -gt 0 ]]; then
    engine_args+=("${EXTRA_ARGS[@]}")
fi

portable_timeout() {
    local secs="$1"; shift
    if command -v timeout >/dev/null 2>&1; then
        timeout "$secs" "$@"; return $?
    elif command -v gtimeout >/dev/null 2>&1; then
        gtimeout "$secs" "$@"; return $?
    fi
    # Fallback for macOS without coreutils: background child + timer killer
    "$@" &
    local child=$!
    ( sleep "$secs"; kill "$child" 2>/dev/null ) &
    local timer=$!
    wait "$child"
    local rc=$?
    kill "$timer" 2>/dev/null
    wait "$timer" 2>/dev/null
    [[ $rc -eq 143 ]] && return 124  # SIGTERM -> match timeout(1) exit code
    return $rc
}

PY_PID=""
cleanup() {
    if [[ -n "$PY_PID" ]]; then
        kill "$PY_PID" 2>/dev/null || true
        wait "$PY_PID" 2>/dev/null || true
    fi
    if [[ -n "$RUNTIME_MODE_DIR_PATH" && -d "$RUNTIME_MODE_DIR_PATH" ]]; then
        rm -rf "$RUNTIME_MODE_DIR_PATH"
    fi
    print_shadow_summary
}
trap cleanup EXIT INT TERM

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
        --regime-model-path "$REGIME_MODEL_PATH"
        --train-ticks "$TRAIN_TICKS"
        --train-epochs "$TRAIN_EPOCHS"
        --continuous-train-every-ticks "$CONTINUOUS_TRAIN_EVERY_TICKS"
        --continuous-train-window-ticks "$CONTINUOUS_TRAIN_WINDOW_TICKS"
        --safe-mode-ticks "$SAFE_MODE_TICKS"
        --drift-min-samples "$DRIFT_MIN_SAMPLES"
        --drift-ic-floor "$DRIFT_IC_FLOOR"
        --no-require-full-model-stack
        --log-path "$ALPHA_LOG_PATH"
    )

    log_info "Starting python shadow session with $PYTHON_BIN (symbol=$SYMBOL, exchanges=$ALPHA_EXCHANGES, signals -> $SIGNAL_FILE, log=$ALPHA_LOG_PATH)"
    (cd "$REPO_ROOT" && "$PYTHON_BIN" "${ALPHA_ARGS[@]}") &
    PY_PID=$!
fi

log_info "Resolved startup strategy_mode=$STRATEGY_MODE futures_config=$FUTURES_CONFIG position_mode=${FUTURES_POSITION_MODE:-n/a} flip_enabled=${FUTURES_FLIP_ENABLED:-n/a} flip_min_abs_signal_bps=${FUTURES_FLIP_MIN_ABS_SIGNAL_BPS:-n/a}"
log_info "Starting C++ shadow runner symbol=$SYMBOL venues=$VENUES duration=${DURATION_SECS}s interval=${INTERVAL_MS}ms runtime_mode_dir=$RUNTIME_MODE_DIR_NAME"

if [[ ! -x "$ENGINE_BIN" ]]; then
    log_error "Missing trading_engine binary at $ENGINE_BIN"
    log_info "Build first: mkdir -p build && cd build && cmake .. && make -j\$(nproc)"
    exit 1
fi

if [[ "$RUN_ONCE" -eq 1 ]]; then
    DURATION_SECS="$(awk "BEGIN { printf \"%.3f\", ($INTERVAL_MS * 2)/1000 }")"
fi

# Run the engine, filtering out noisy libwebsockets demo-handler warnings that
# are always emitted when those optional LWS extensions (SSH, raw-file, upload)
# are compiled in but not configured.  Real WARNs from trading code are kept.
_LWS_NOISE_PATTERN="lws-test-sshd-server-key|ssh pvo.*is mandatory|Missing pvo.*fifo-path|requires.*upload-dir"

engine_exit=0
if portable_timeout "$DURATION_SECS" "$ENGINE_BIN" "${engine_args[@]}" \
        2> >(grep -Ev "$_LWS_NOISE_PATTERN" >&2); then
    :
else
    engine_exit=$?
    if [[ "$engine_exit" -ne 124 ]]; then
        log_error "trading_engine exited with status $engine_exit"
        exit "$engine_exit"
    fi
fi

log_info "Completed shadow run window"
