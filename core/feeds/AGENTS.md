# core/feeds/

Exchange feed handlers and shared market-data normalization utilities.

## Layout

- `binance/` — Binance feed handler.
- `kraken/` — Kraken feed handler.
- `okx/` — OKX feed handler.
- `coinbase/` — Coinbase Advanced Trade feed handler.
- `common/` — shared adapters/utilities (`book_manager.hpp`, `tick_size.hpp`).

## Classes & Methods (Quick Reference)

- **`BookManager` (`common/book_manager.hpp`)** — Bridges normalized snapshots/deltas into `OrderBook`.
  - `snapshot_handler()/delta_handler()`: Callbacks for feed handlers to apply updates.
  - `age_ms()`: Freshness metric used by risk checks.
  - `set_publisher(LobPublisher*)`: Enables optional shared-memory top-of-book publishing.

- **`BinanceFeedHandler` (`binance/binance_feed_handler.hpp`)**
  - `start()/stop()`: Starts/stops snapshot + websocket synchronization flow.
  - `process_message(...)`: Parses stream payloads and emits snapshot/delta callbacks.
  - `sync_stats()`: Returns resync/buffer telemetry.

- **`KrakenFeedHandler` (`kraken/kraken_feed_handler.hpp`)**
  - `start()/stop()`: Controls handler lifecycle.
  - `process_message(...)`: Parses/apply snapshot-delta messages and sequencing logic.
  - `crc32_for_test(...)`: Deterministic checksum helper used in tests.

- **`OkxFeedHandler` (`okx/okx_feed_handler.hpp`)**
  - `start()/stop()`: Controls WS loop and synchronization states.
  - `process_message(...)`: Handles snapshot/delta parsing and ordering.
  - `validate_delta_sequence(...)` (internal): Guards sequence continuity.

- **`CoinbaseFeedHandler` (`coinbase/coinbase_feed_handler.hpp`)**
  - `start()/stop()`: Controls auth, subscription, and lifecycle.
  - `process_message(...)`: Converts venue messages to normalized updates.
  - `build_subscription_messages()/generate_jwt(...)`: Builds authenticated WS subscriptions.

- **`tick_from_string(...)` (`common/tick_size.hpp`)**
  - Parses exchange tick-size strings into stable `double` increments.
