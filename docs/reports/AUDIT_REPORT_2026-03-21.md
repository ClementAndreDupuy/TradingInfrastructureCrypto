# Feed Audit Report — 2026-03-21

## Scope
Deep audit of the four C++ market-data feeds in `core/feeds/` (Binance, Kraken, OKX, Coinbase) plus the feed-to-bridge wiring in `core/engine/trading_engine_main.cpp`.

## Findings

### Critical
1. **Feed-to-bridge startup ordering dropped the initial snapshot and early deltas.**
   - `trading_engine_main.cpp` started all feed handlers before registering `BookManager` snapshot/delta callbacks.
   - Because each handler transitions to streaming during `start()`, the shared books and LOB bridge missed the first synchronized snapshot and any early deltas emitted before callback registration.
   - This directly explains a “connection established but no data reaches the bridge” failure mode.

### High
2. **Coinbase feed incorrectly required credentials for public `level2` market data.**
   - The handler hard-failed when `COINBASE_API_KEY` / `COINBASE_API_SECRET` were absent or malformed.
   - Coinbase Advanced Trade `level2` market data can be consumed unauthenticated; credentials improve reliability but should not be a hard prerequisite.
   - The old behavior unnecessarily disabled the feed and forced research-only fallback behavior.

3. **Kraken WebSocket path parsing ignored the configured URL path.**
   - The handler always connected to `/v2` even when a custom `ws_url` path was supplied.
   - The default worked, but custom endpoints/tests were not honored.

### Medium
4. **Book grids were initialized from fallback tick sizes instead of exchange-derived tick sizes.**
   - The engine computed `BookManager` grid sizes from `handler.tick_size()` before any synchronous tick-size refresh occurred, so the value was still zero and the fallback grid was used.
   - This did not always break the feed, but it weakened cross-venue book fidelity and made the initial bridge state less reliable.

## Venue-by-venue protocol review

### Binance
- Snapshot+delta handoff matches Binance’s documented `lastUpdateId` / `U` / `u` contract.
- Buffering, stale-update skip, and bridge-delta enforcement are implemented.
- No small protocol bug was changed in the feed handler itself during this pass.

### Kraken
- Handler consumes WebSocket v2 `book` snapshot/update messages and validates CRC32 over the maintained top-of-book state.
- Local monotonic sequencing is used because the channel does not provide a venue sequence suitable for direct cross-message continuity checks.
- Fixed a small transport bug: the configured WebSocket path is now honored.

### OKX
- Handler follows snapshot/update semantics, `seqId`/`prevSeqId` continuity, and checksum validation.
- No small correctness bug was identified that warranted a targeted code change during this pass.

### Coinbase
- Handler correctly standardizes on Advanced Trade `level2` subscribe and `l2_data` processing.
- The main correctness issue was the startup auth gate, which is now relaxed to allow public market-data sessions.

## Remediation completed
- Registered feed callbacks before starting feed threads.
- Added an explicit pre-start tick-size refresh so books are constructed with venue-derived grids when available.
- Allowed Coinbase to start without credentials, while still using JWT auth when available.
- Honored custom Kraken WebSocket paths.

## Remaining watch items
- Binance should still be stress-checked under burst traffic because the live symptom you reported can also occur when startup ordering hides the first valid snapshot.
- Coinbase symbol normalization should be kept under review if the runtime symbol universe mixes USD and USDT products across venues.
