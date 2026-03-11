#!/bin/bash
# Manual build script (no CMake).
# For production use CMake: mkdir -p build && cd build && cmake .. && make -j$(nproc)

set -e

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_ROOT"

echo "=== Building ThamesRiverTrading ==="

rm -rf build_manual
mkdir -p build_manual

echo "Compiling Binance feed handler..."
clang++ -std=c++17 -O2 -Wall -I. \
    -c core/feeds/binance/binance_feed_handler.cpp \
    -o build_manual/binance_feed_handler.o

echo "Compiling Kraken feed handler..."
clang++ -std=c++17 -O2 -Wall -I. \
    -c core/feeds/kraken/kraken_feed_handler.cpp \
    -o build_manual/kraken_feed_handler.o

echo "Compiling execution connectors..."
clang++ -std=c++17 -O2 -Wall -I. \
    -c core/execution/binance_connector.cpp \
    -o build_manual/binance_connector.o
clang++ -std=c++17 -O2 -Wall -I. \
    -c core/execution/kraken_connector.cpp \
    -o build_manual/kraken_connector.o

echo "Compiling perp_arb strategy..."
clang++ -std=c++17 -O2 -Wall -I. \
    -c core/strategy/perp_arb_main.cpp \
    -o build_manual/perp_arb_main.o

echo "Linking perp_arb..."
clang++ -std=c++17 \
    build_manual/binance_feed_handler.o \
    build_manual/kraken_feed_handler.o \
    build_manual/binance_connector.o \
    build_manual/kraken_connector.o \
    build_manual/perp_arb_main.o \
    -lssl -lcrypto \
    -o build_manual/perp_arb

echo ""
echo "✓ Build complete: build_manual/perp_arb"
echo ""
echo "Shadow mode (safe default):"
echo "  ./build_manual/perp_arb --shadow"
echo ""
echo "Live mode (requires API keys and 2+ weeks shadow):"
echo "  export BINANCE_API_KEY='...' BINANCE_API_SECRET='...'"
echo "  export KRAKEN_API_KEY='...'  KRAKEN_API_SECRET='...'"
echo "  ./build_manual/perp_arb --live"
