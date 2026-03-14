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
- [x] **C9** `core/execution/` — Implemented live exchange connectors for **Binance/OKX/Kraken/Coinbase** (`submit_order`, `cancel_order`, `cancel_all`, `reconcile`) with authenticated signing (HMAC SHA256 when OpenSSL is available), retries, idempotency keys, and exchange order-id mapping
- [x] **C10** `CMakeLists.txt` + `core/` + `deploy/systemd/trading-engine.service` — Added real `trading_engine` executable target and entrypoint (`main`) wiring books + risk + execution + alpha-gated SOR path for live/shadow runtime; service now points to built binary with mode/venues/symbol args
- [ ] **C11** `deploy/run_live.sh` + `deploy/run_shadow.sh` + deployment configs — Replace single-venue/SOLANA assumptions with explicit four-exchange orchestration (BINANCE/KRAKEN/OKX/COINBASE) and venue-specific credentials

### HIGH — Required for Production Quality

- [x] **H1** `core/common/logging.hpp` — Replace `std::cout` with async ring-buffer logger (spdlog async mode or custom lock-free SPSC ring); zero allocation on hot path
- [x] **H2** `core/feeds/binance/binance_feed_handler.cpp` + `kraken_feed_handler.cpp` — WebSocket ping frames sent every 30 s (Binance) / 20 s (Kraken) from stream phase; pong handled automatically by libwebsockets
- [x] **H3** `core/risk/` — Implement circuit breaker: order rate limiter, max daily loss breaker, stale book detector, message rate guard — use params already in `config/dev/risk.yaml`
- [x] **H4** `core/feeds/coinbase/` — Created Coinbase Advanced Trade WebSocket feed handler (L2 order book channel) with snapshot/delta sync and strict sequence continuity checks
- [x] **H5** `core/feeds/okx/` — Created OKX WebSocket feed handler (`books` channel) with REST snapshot + WebSocket delta sync and sequence continuity validation
- [x] **H6** `CMakeLists.txt` — Added `libwebsockets` via `pkg_check_modules`; linked to both `binance_feed` and `kraken_feed` targets; libcurl and nlohmann/json handled in `core/CMakeLists.txt`; pybind11 handled via `bindings/setup.py`
- [x] **H7** `tests/` — Create feed replay test suite: record live feed messages to file, replay deterministically, compare order-book state byte-for-byte across runs
- [x] **H8** `tests/` — Add integration tests for full pipeline: feed handler → book manager → market maker → shadow engine (end-to-end with recorded data)
- [ ] **H9** `core/feeds/okx/okx_feed_handler.cpp` — Add required OKX `books` checksum (CRC32) verification and forced resync on mismatch
- [ ] **H10** `core/feeds/coinbase/` — Align Coinbase market-data protocol with project standard (full depth fidelity target vs current level2 stream); document chosen protocol and invariants
- [ ] **H11** `core/execution/market_maker.hpp` + `core/risk/circuit_breaker.hpp` — Wire `CircuitBreaker` checks directly into pre-submit order path (rate, stale book, drawdown, price-deviation, consecutive-loss guards)

### MEDIUM — Quality and Correctness Gaps

- [x] **M1** `research/alpha/neural_alpha/backtest.py` — Fix Sharpe calculation: use actual timestamp-based equity curve returns (annualize on time elapsed, not trade count proxy)
- [x] **M2** `research/alpha/neural_alpha/backtest.py` — Add queue position simulation (Poisson arrival model, fill probability by queue depth and size)
- [x] **M3** `research/alpha/neural_alpha/features.py` — Improve adverse-selection label: replace sign-flip heuristic with fill-reversion model (check if price reverts against fill direction within N ticks)
- [x] **M4** `research/alpha/neural_alpha/backtest.py` — Add basic market impact model (linear or square-root impact based on order size / ADV)
- [x] **M5** `core/common/logging.hpp` — Replace `std::chrono::system_clock` with PTP-synchronized clock or at minimum RDTSC-based timestamps for sub-microsecond precision
- [ ] **M6** `core/execution/order_manager.hpp` lines 264–272 — Fix entry price VWAP: `new_avg = (old_avg * old_qty + fill_px * fill_qty) / (old_qty + fill_qty)`; remove `1e-12` epsilon hack
- [x] **M7** `core/feeds/binance/binance_feed_handler.cpp` — Added REST rate limit tracking: minimum 1s between snapshot calls; HTTP 429/418 responses trigger 60s backoff
- [x] **M8** `core/shadow/shadow_engine.hpp` — Make fee structure config-driven: read from `config/dev/risk.yaml` instead of hardcoded `2.0 bps` maker / `5.0 bps` taker
- [ ] **M9** `tests/unit/` — Add negative tests for feed handlers: malformed JSON, out-of-order sequences, duplicate sequence IDs, extreme price levels
- [ ] **M10** `tests/unit/test_neural_alpha.py` — Add edge case tests: walk-forward with very small dataset, invalid input tensor shapes, NaN propagation through model
- [ ] **M11** `tests/` + CI — Add deterministic connector contract tests (submit/cancel/reconcile state machine) and replay-based disconnect/reconnect chaos tests per exchange
- [ ] **M12** `tests/perf/` + CI — Add latency benchmark gates enforcing documented budgets (`orderbook<1us`, `risk<1us`, `feed<10us`) with fail thresholds
- [ ] **M13** `scripts/` + docs — Add preflight dependency checker for local/prod builds (libwebsockets/libcurl/json) and fail-fast bootstrap diagnostics

### LOW — Nice-to-Have

- [ ] **L1** `core/orderbook/orderbook.hpp` — Add guard against pathologically wide spreads (e.g., snapshot with $1M range from malformed data)
- [ ] **L2** `core/orderbook/orderbook.hpp` — Add Binance depth snapshot checksum validation (Binance provides checksum field in snapshot response)
- [ ] **L3** `research/alpha/neural_alpha/features.py` — Expand per-level queue imbalance to all N price levels beyond top 5 (already noted in feature TODO comments)
- [ ] **L4** `deploy/daily_train.py` — Add alerting webhook (Slack/PagerDuty) when model fails to promote or IC drops below floor
- [ ] **L5** `core/execution/market_maker.hpp` — Add inventory skew decay: reduce skew magnitude as position approaches zero to avoid over-trading a flat book
- [ ] **L6** `tests/` — Add C++ latency benchmark tests that measure actual order-book delta and risk check timing; fail CI if over budget defined in AGENTS.md
- [ ] **L7** General — Add `.clang-tidy` and `.clang-format` configs; enforce in CI pre-commit hook

### RESEARCH - Needs investigating, come with a plan to implement it for our server

- [ ] **R1** — Research on integrate On-Chain metrics (Netflow, Spent Output Profit Ratio, Net Unrealised Profit/Loss, Whale transaction analysis, Defi Protocol metrics...)
- [x] **R2** — Research completed in `agents/reports/RESEARCH_R2_MARKET_REGIME_IDENTIFICATION_2026-03-14.md`: State-of-the-art real-time market regime identification and implementation blueprint for hybrid CPD + HMM/HSMM + microstructure overlays
- [ ] **R3** — Research on deep reinforcement learning for autonomous execution (State Space Design, Action Space design, Reward function formulation)
- [x] **R4** — Research completed in `agents/reports/RESEARCH_R4_SMART_ORDER_ROUTING_2026-03-14.md`: Smart Order Routing plan (Binance/Kraken/OKX/Coinbase) and implementation path unblocking C9/C10
- [ ] **R5** — Research on hardware execution with Field-Programmable Gate Arrays and co-locating servers in the same clusters as exchanges 
