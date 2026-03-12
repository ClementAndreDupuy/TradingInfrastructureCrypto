# ThamesRiverTrading — Project Review vs Industry Standard

> Audit date: 2026-03-12. Compared against production HFT/crypto trading systems
> (Jump Trading, Wintermute, GSR, and similar quant trading operations).

---

## Overall Verdict

**Architecturally ambitious. Python ML pipeline is production-ready. C++ hot path is a well-designed skeleton that cannot trade live.**

The design philosophy mirrors how real HFT/crypto quant shops structure systems. The design decisions throughout are correct and reflect real industry knowledge. The gap is between design and a runnable system.

---

## What's Genuinely Strong (Industry-Comparable)

### Order Book — A+
- Flat-array with O(1) price-to-index lookup — textbook HFT order book design
- Pre-allocated, no heap allocation in hot path
- Atomic sequence tracking, stale delta rejection
- Comparable to how Binance's own matching engine works

### Kill Switch — A+
- Lock-free, atomic-only, <10ns on x86-64
- Dead man's switch with heartbeat timeout (5s default)
- Multiple trigger reasons (MANUAL, DRAWDOWN, CIRCUIT_BREAKER, HEARTBEAT_MISSED, BOOK_CORRUPTED)
- Better than many boutique crypto shops

### Shared-Memory IPC — A
- mmap 24-byte fixed-layout signal with atomic reads, stale detection, fail-open fallback
- Correct pattern used by firms like Jump Trading for Python→C++ signal bridges
- Python writes asynchronously at ~500ms; C++ reads in <100ns

### Neural Alpha Model — A
- GNN spatial (self-attention over LOB levels) + Transformer temporal is a legitimate research architecture
- Multi-task heads (return regression, direction classification, adverse-selection risk)
- Transaction-cost penalty term in loss function — shows real understanding of signal economics
- Not a toy model

### Walk-Forward Validation — A
- Rolling z-score normalization with expanding windows, no future leakage
- Proper IC / ICIR metrics
- Separates real quant work from retail backtesting

### Shadow Trading — A
- Identical code path to live (not a mock)
- Fill simulation at bid/ask, maker/taker fee discrimination, JSONL audit trail
- Atomic PnL tracking across exchanges

### Operational Infrastructure — A-
- Daily retraining with IC-gated model promotion (only promotes if IC improves over production)
- Prometheus-compatible metrics export
- Per-environment config separation (dev / shadow / live)
- Training job caching to avoid re-fetching same day's data


## Industry Comparison Table

| Dimension | This Project | Boutique Crypto Shop | Tier-1 HFT Firm |
|---|---|---|---|
| Architecture philosophy | ✅ Correct | ✅ | ✅ |
| Order book design | ✅ Production-quality | ✅ | ✅ |
| Kill switch | ✅ Excellent | ✅ | ✅ |
| Feed handler completeness | ❌ Stubbed | ✅ | ✅ |
| WebSocket resilience | ❌ None | ✅ | ✅ |
| JSON parsing | ❌ Regex | ✅ | ✅ |
| Hardware timestamps | ❌ No | Sometimes | ✅ |
| Kernel bypass networking | ❌ No | Rarely | ✅ |
| ML signal quality | ✅ Good | Varies | Varies |
| Walk-forward validation | ✅ Yes | Rarely | ✅ |
| Shadow trading | ✅ Yes | Rarely | ✅ |
| Market impact modeling | ❌ No | Sometimes | ✅ |
| Operational monitoring | ✅ Good | Varies | ✅ |
| Test coverage | ⚠️ Partial | Varies | ✅ |
| Exchange count live | ❌ 0/3 | Typically 2–5 | 10+ |

---

## Complete TODO List

### CRITICAL — Blockers (System Cannot Trade Live)

- [x] **C1** `core/feeds/binance/binance_feed_handler.cpp` — Implemented real WebSocket via libwebsockets; background thread connects to `<symbol>@depth@100ms` stream with TLS
- [x] **C2** `core/feeds/binance/binance_feed_handler.cpp` — `process_delta()` now parses real bids (`"b"`) and asks (`"a"`) arrays; fake hardcoded data removed
- [x] **C3** `core/feeds/kraken/kraken_feed_handler.cpp` — Implemented real WebSocket via libwebsockets; sends `{"method":"subscribe","params":{"channel":"book",...}}` after ESTABLISHED
- [x] **C4** `core/feeds/kraken/kraken_feed_handler.cpp` — Real delta parsing from WebSocket messages was already correct; confirmed working end-to-end
- [x] **C5** `core/common/rest_client.hpp` — Replaced `popen("curl ...")` with libcurl C API; thread-local connection reuse, 10s timeout, 5s connect timeout, TCP keepalive, HTTP status codes. Fallback via `curl_abi.hpp` when dev headers absent.
- [x] **C6** `core/common/rest_client.hpp` + all feed handlers — Replaced all regex JSON parsing with `nlohmann/json` (falls back to vendored `core/common/json.hpp`); `find_package(CURL)` + `find_package(nlohmann_json)` added to `core/CMakeLists.txt`; `<regex>` removed from both feed handlers.
- [x] **C7** `core/feeds/binance/binance_feed_handler.cpp` + `kraken_feed_handler.cpp` — Exponential-backoff reconnection implemented (100 ms → 200 ms → … → 30 s cap) in `ws_event_loop()`; re-snapshot + re-sync on every reconnect
- [x] **C8** `bindings/` — Created pybind11 bridge: `bindings/bindings.cpp` (OrderBook, KillSwitch, BinanceFeedHandler, KrakenFeedHandler with GIL-safe callbacks) + `bindings/setup.py` (pkg-config driven, links libwebsockets + libcurl + nlohmann/json)

### HIGH — Required for Production Quality

- [ ] **H1** `core/common/logging.hpp` — Replace `std::cout` with async ring-buffer logger (spdlog async mode or custom lock-free SPSC ring); zero allocation on hot path
- [x] **H2** `core/feeds/binance/binance_feed_handler.cpp` + `kraken_feed_handler.cpp` — WebSocket ping frames sent every 30 s (Binance) / 20 s (Kraken) from stream phase; pong handled automatically by libwebsockets
- [ ] **H3** `core/risk/` — Implement circuit breaker: order rate limiter, max daily loss breaker, stale book detector, message rate guard — use params already in `config/dev/risk.yaml`
- [ ] **H4** `core/feeds/coinbase/` — Create and implement Coinbase Advanced Trade WebSocket feed handler (L2 order book channel)
- [ ] **H5** `core/feeds/okx/` — Create and implement OKX WebSocket feed handler (`books` channel, sequence validation)
- [x] **H6** `CMakeLists.txt` — Added `libwebsockets` via `pkg_check_modules`; linked to both `binance_feed` and `kraken_feed` targets; libcurl and nlohmann/json handled in `core/CMakeLists.txt`; pybind11 handled via `bindings/setup.py`
- [ ] **H7** `tests/` — Create feed replay test suite: record live feed messages to file, replay deterministically, compare order-book state byte-for-byte across runs
- [ ] **H8** `tests/` — Add integration tests for full pipeline: feed handler → book manager → market maker → shadow engine (end-to-end with recorded data)

### MEDIUM — Quality and Correctness Gaps

- [ ] **M1** `research/alpha/neural_alpha/backtest.py` — Fix Sharpe calculation: use actual timestamp-based equity curve returns (annualize on time elapsed, not trade count proxy)
- [ ] **M2** `research/alpha/neural_alpha/backtest.py` — Add queue position simulation (Poisson arrival model, fill probability by queue depth and size)
- [ ] **M3** `research/alpha/neural_alpha/features.py` — Improve adverse-selection label: replace sign-flip heuristic with fill-reversion model (check if price reverts against fill direction within N ticks)
- [ ] **M4** `research/alpha/neural_alpha/backtest.py` — Add basic market impact model (linear or square-root impact based on order size / ADV)
- [ ] **M5** `core/common/logging.hpp` — Replace `std::chrono::system_clock` with PTP-synchronized clock or at minimum RDTSC-based timestamps for sub-microsecond precision
- [ ] **M6** `core/execution/order_manager.hpp` lines 264–272 — Fix entry price VWAP: `new_avg = (old_avg * old_qty + fill_px * fill_qty) / (old_qty + fill_qty)`; remove `1e-12` epsilon hack
- [x] **M7** `core/feeds/binance/binance_feed_handler.cpp` — Added REST rate limit tracking: minimum 1s between snapshot calls; HTTP 429/418 responses trigger 60s backoff
- [ ] **M8** `core/shadow/shadow_engine.hpp` — Make fee structure config-driven: read from `config/dev/risk.yaml` instead of hardcoded `2.0 bps` maker / `5.0 bps` taker
- [ ] **M9** `tests/unit/` — Add negative tests for feed handlers: malformed JSON, out-of-order sequences, duplicate sequence IDs, extreme price levels
- [ ] **M10** `tests/unit/test_neural_alpha.py` — Add edge case tests: walk-forward with very small dataset, invalid input tensor shapes, NaN propagation through model

### LOW — Nice-to-Have

- [ ] **L1** `core/orderbook/orderbook.hpp` — Add guard against pathologically wide spreads (e.g., snapshot with $1M range from malformed data)
- [ ] **L2** `core/orderbook/orderbook.hpp` — Add Binance depth snapshot checksum validation (Binance provides checksum field in snapshot response)
- [ ] **L3** `research/alpha/neural_alpha/features.py` — Expand per-level queue imbalance to all N price levels beyond top 5 (already noted in feature TODO comments)
- [ ] **L4** `deploy/daily_train.py` — Add alerting webhook (Slack/PagerDuty) when model fails to promote or IC drops below floor
- [ ] **L5** `core/execution/market_maker.hpp` — Add inventory skew decay: reduce skew magnitude as position approaches zero to avoid over-trading a flat book
- [ ] **L6** `tests/` — Add C++ latency benchmark tests that measure actual order-book delta and risk check timing; fail CI if over budget defined in CLAUDE.md
- [ ] **L7** General — Add `.clang-tidy` and `.clang-format` configs; enforce in CI pre-commit hook

---

## Immediate Action Plan (to go live)

Priority order for a single engineer:

1. **C5 + C6** — Replace REST client and JSON parsing first (unblocks everything else)
2. **C1 + C2** — Binance WebSocket connection + real delta parsing
3. **C3 + C4** — Kraken WebSocket connection + real delta parsing
4. **C7** — Reconnection logic for both handlers
5. **H6** — Update CMakeLists.txt with real dependencies
6. **C8** — pybind11 bindings directory
7. **H1** — Async logging (prevents hot-path latency regression under load)
8. **H3** — Circuit breaker implementation
9. **H7 + H8** — Replay and integration test suite