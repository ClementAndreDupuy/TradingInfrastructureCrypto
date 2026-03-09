#!/bin/bash

set -e

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_ROOT"

echo "=== Building ThamesRiverTrading ==="

# Clean previous build
rm -rf build_manual
mkdir -p build_manual

# Compile common (header-only, no compilation needed)
echo "✓ Common types (header-only)"

# Compile Binance feed handler
echo "Compiling Binance feed handler..."
clang++ -std=c++17 -O2 -Wall -I. \
    -c core/feeds/binance/binance_feed_handler.cpp \
    -o build_manual/binance_feed_handler.o

# Compile example
echo "Compiling example..."
clang++ -std=c++17 -O2 -Wall -I. \
    -c core/feeds/binance/example_main.cpp \
    -o build_manual/example_main.o

# Link example
echo "Linking binance_feed_example..."
clang++ -std=c++17 \
    build_manual/binance_feed_handler.o \
    build_manual/example_main.o \
    -o build_manual/binance_feed_example

echo "✓ Build complete!"
echo ""
echo "Run example:"
echo "  export BINANCE_API_KEY='your_key'"
echo "  export BINANCE_API_SECRET='your_secret'"
echo "  ./build_manual/binance_feed_example"
