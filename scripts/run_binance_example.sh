#!/bin/bash

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Load Binance API credentials from config
export BINANCE_API_KEY="j67tC11UUBc0gJhhkQoQddAY91N7aNmdRDIvoXJGMo53Aq6maaUkRDbmwCmt6qOJ"
export BINANCE_API_SECRET="YVqYJzDom7C3rdoBUoSWXDsC4MbZwKLyqcpsBSmTtziqWk0tZW7CTFcTzz4plSmj"

echo "=== Binance Feed Handler Example ==="
echo "API Key loaded: ${BINANCE_API_KEY:0:10}..."
echo ""

# Check if binary exists
if [ ! -f "$PROJECT_ROOT/build_manual/binance_feed_example" ]; then
    echo "Error: binance_feed_example not built yet"
    echo "Please run: ./scripts/build.sh"
    exit 1
fi

# Run the example
"$PROJECT_ROOT/build_manual/binance_feed_example"
