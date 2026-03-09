# Binance Feed Handler

WebSocket feed handler for Binance order book depth updates.

## Features

- WebSocket depth stream (`@depth@100ms`)
- REST API snapshot with delta buffering
- Sequence validation: `U <= lastUpdateId+1 <= u`
- Gap detection and re-snapshot triggering
- Thread-safe state management

## Protocol

**Snapshot:** `GET /api/v3/depth?symbol=BTCUSDT&limit=1000`

**Delta Stream:** `wss://stream.binance.com:9443/ws/btcusdt@depth@100ms`

## Sequence Validation

Binance rule: `U <= lastUpdateId + 1 <= u`

Where `lastUpdateId` is from snapshot, `U` is first update ID, `u` is last update ID. Gaps trigger re-snapshot.

## Usage

```cpp
#include "binance_feed_handler.hpp"

using namespace trading;

int main() {
    BinanceFeedHandler handler("BTCUSDT");

    handler.set_snapshot_callback([](const Snapshot& s) {
        std::cout << "Snapshot received" << std::endl;
    });

    handler.set_delta_callback([](const Delta& d) {
        std::cout << "Delta: " << d.price << " @ " << d.size << std::endl;
    });

    handler.start();
    handler.stop();
    return 0;
}
```

## API Key Configuration

The handler automatically loads API credentials from environment variables:

```bash
export BINANCE_API_KEY="your_api_key"
export BINANCE_API_SECRET="your_api_secret"
./bin/binance_feed_example
```

Or pass directly (not recommended for production):

```cpp
BinanceFeedHandler handler("BTCUSDT", "api_key", "api_secret");
```

Public market data endpoints work without authentication but have lower rate limits.

## Build

### Option 1: Using build script (Recommended)

```bash
# Build
./scripts/build.sh

# Run with API keys from config
./scripts/run_binance_example.sh

# Or run manually with env vars
export BINANCE_API_KEY="your_key"
export BINANCE_API_SECRET="your_secret"
./build_manual/binance_feed_example
```

### Option 2: Using CMake

```bash
mkdir build && cd build
cmake .. && make -j$(nproc)
./bin/binance_feed_example
./tests/unit/binance_feed_test
```

## State Machine

DISCONNECTED → CONNECTING → BUFFERING → SYNCHRONIZED → STREAMING

Sequence gaps trigger return to BUFFERING.

## Testing

```bash
./tests/unit/binance_feed_test
```

Tests: handler lifecycle, sequence validation, delta buffering, gap detection.

## Current Implementation

✅ **REST API** - Real-time snapshot fetching from Binance
✅ **JSON Parsing** - Integrated parser for order book data
✅ **HTTP Client** - Built-in curl-based client
✅ **API Key Support** - Authenticated requests via environment variables
✅ **Compact Design** - All functionality in single handler class

## Production Requirements

- Replace curl with libcurl C++ library for better performance
- Replace regex JSON parsing with RapidJSON
- Implement WebSocket with Boost.Beast for real-time deltas
- Use PTP hardware clock for timestamps
- Add exponential backoff and ping/pong keepalive
- Prometheus metrics for latency and message rate
- Target: < 10μs processing latency
