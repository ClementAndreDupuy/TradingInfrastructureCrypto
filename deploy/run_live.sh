#!/usr/bin/env bash
# Live trading — four-venue SOR runtime (Binance, Kraken, OKX, Coinbase).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENGINE_BIN="$REPO_ROOT/build/bin/trading_engine"

if [[ ! -x "$ENGINE_BIN" ]]; then
    echo "[live] ERROR: missing trading_engine binary at $ENGINE_BIN"
    echo "[live] Build first: mkdir -p build && cd build && cmake .. && make -j\$(nproc)"
    exit 1
fi

for var in BINANCE_API_KEY BINANCE_API_SECRET KRAKEN_API_KEY KRAKEN_API_SECRET OKX_API_KEY OKX_API_SECRET COINBASE_API_KEY COINBASE_API_SECRET; do
    if [[ -z "${!var:-}" ]]; then
        echo "[live] ERROR: required env var $var is not set"
        exit 1
    fi
done

exec "$ENGINE_BIN" "$@"
