## TODO List

### CRITICAL
- [x] **C-NEW-1** `core/engine/trading_engine_main.cpp` — Replaced one-shot demo harness with a production daemon loop using one alpha signal and Smart Order Router fanout across enabled venues, with continuous operation, kill-switch heartbeat keepalive, reconnect-triggered reconciliation, periodic reconciliation every 30 s, and clean SIGTERM/SIGINT shutdown handling.
  - acceptance: binary runs continuously until SIGTERM; does not exit on clean operation
  - acceptance: kill switch heartbeat is refreshed at ≥ 1 Hz
  - acceptance: reconciliation runs on reconnect and every 30 s
- [x] **C-NEW-2** `deploy/systemd/neural-alpha-shadow.service` — Fixed `--exchanges SOLANA` to `--exchanges BINANCE,KRAKEN` so shadow validation is collected on intended live venues.
- [x] **C-NEW-3** `deploy/aws/userdata.sh` — Updated bootstrap to load and write Binance, Kraken, OKX, and Coinbase `API_KEY` + `API_SECRET` values into `/etc/trading/env` from Secrets Manager.
- [x] **C1** `core/execution/live_connector_base.hpp` + venue connectors — Replaced generic auth flow with exchange-specific canonical signing (Binance/Kraken/OKX/Coinbase), removed non-cryptographic signature fallback from live path, and hard-fail connector `connect()` when OpenSSL backend is unavailable
- [x] **C2** `core/execution/*_connector.cpp` — Implement authenticated cancel/replace/query endpoints with strict response parsing; stop using synthetic venue order IDs and persist real exchange IDs *(submit/cancel/replace/query now implemented with strict parsing and real venue IDs persisted in `VenueOrderMap`)*
- [x] **C3** `core/execution/` — deliver production reconciliation for open orders/fills/balances/positions against internal canonical state with deterministic mismatch classification and actions; reconnect + periodic loops share one reconciliation engine in `ReconciliationService`
  - acceptance: deterministic state diff between venue snapshots and `OrderManager` + internal ledgers
  - acceptance: reconnect bootstrap and periodic drift loops both run same reconciliation logic
  - acceptance: mismatch classes are explicit (`missing_order`, `qty_drift`, `fill_gap`, `balance_drift`, `position_drift`) with deterministic actions
- [x] **C4** `core/execution/` + `core/risk/` — Added durable idempotency journal with on-disk replay and deterministic operation state transitions; integrated execution recovery for retry storms, duplicate acks, and cancel/replace race conflicts plus risk-level `RecoveryGuard` kill-switch escalation
- [x] **C5** `core/execution/reconciliation_service.hpp` + `core/execution/*/` — Implemented fill reconciliation pipeline with trade-history ingestion, stable dedupe keys, cumulative qty/notional/fee checks, and deterministic ledger replay
- [x] **C6** `core/execution/reconciliation_service.hpp` + `tests/` — Added staged drift remediation policy (retry budgets, severity levels, explicit cancel-all/risk-halt hooks, incident trail) plus failure-injection coverage for reconnect/drift edge cases

### HIGH
- [x] **H1** `core/risk/` — Added `GlobalRiskControls` with config-driven portfolio/global risk caps (gross/net notional, concentration, venue caps, cross-venue netting), wired into trading engine pre-trade flow via `RiskConfigLoader`, and covered with dedicated unit tests
- [x] **H2** `core/shadow/shadow_engine.hpp` — Extended shadow simulator realism with queue-position decay, partial fills, deterministic latency + slippage modeling, and venue-specific fee modeling for OKX/Coinbase; added dedicated `shadow_engine_test` unit coverage and build-script test wiring
- [x] **H3** `core/execution/smart_order_router.cpp` — Upgraded SOR objective to include fill probability, queue priority, adverse-selection/toxicity signals, and dynamic regime adaptation
- [ ] **H4** `deploy/` + monitoring stack — Define production SLOs/error budgets and wire hard alerts for feed integrity, reject spikes, reconciliation drift, and risk trigger frequency
- [x] **H5** `tests/` + CI — Added sanitizer/race-test matrix (ASan/UBSan + TSan) in CI and deterministic connector failure-injection suites across Binance/Kraken/OKX/Coinbase

### MEDIUM
- [x] **M1** `core/orderbook/orderbook.hpp` — Added adaptive order book recentering strategy with streak + hard-breach triggers, preserving in-range liquidity while re-anchoring grid during volatile out-of-range regimes; covered by dedicated unit tests
- [ ] **M2** `core/feeds/` + `tests/replay/` — Build feed-certification replay harness with venue-specific pathological scenarios and acceptance thresholds
- [x] **M3** `research/alpha/neural_alpha/` + deploy — Added model governance with champion/challenger registry, automatic rollback to previous champion, and drift-triggered safe mode in shadow inference
- [x] **M4** `core/ipc/lob_publisher.hpp` (new file) — Create a 10 000-slot × 256-byte mmap ring buffer that publishes L5 LOB snapshots to `/tmp/trt_lob_feed.bin`. Each slot encodes: exchange_id (uint8), symbol (char[15]), timestamp_ns (int64), mid_price (double), bid_price[5] + bid_size[5] + ask_price[5] + ask_size[5] (double[5] each). Write protocol: fill slot at `write_seq % capacity`, then atomic release-store `write_seq + 1` so Python readers see a complete slot before the counter advances. No heap allocation after `open()`.
  - acceptance: `static_assert(sizeof(LobSlot) == 256)`
  - acceptance: Python can parse the file with the struct format `<B15sq` + `d*21` + `64s`
  - acceptance: magic header `"TRTLOB01"` validated on Python open
- [x] **M5** `core/feeds/common/book_manager.hpp` — Add `set_publisher(LobPublisher*)` and a private `publish_lob(int64_t timestamp_ns)` helper. Call `publish_lob` after every successful `apply_snapshot` and `apply_delta`. Pre-allocate `pub_bids_` / `pub_asks_` vectors (capacity 5) as class members to avoid hot-path heap allocation. Publisher pointer is optional; skip silently when null or not open.
  - acceptance: no `std::vector` allocation in delta callback hot path
  - acceptance: `set_publisher(nullptr)` disables publishing without crash
- [x] **M6** `core/engine/trading_engine_main.cpp` — Instantiate `LobPublisher`, call `open()`, and pass it to all four `BookManager` instances via `set_publisher`. Wire `BinanceFeedHandler`, `KrakenFeedHandler`, `OkxFeedHandler`, and `CoinbaseFeedHandler` to their respective `BookManager` snapshot/delta callbacks and call `start()` on each. Depends on **M4**, **M5**, and **C-NEW-1** (production daemon loop).
  - acceptance: all four feed handlers running; `/tmp/trt_lob_feed.bin` write_seq increments under live traffic
  - acceptance: fallback warning logged when `lob_publisher.open()` fails; engine still starts
- [x] **M7** `research/alpha/neural_alpha/core_bridge.py` (new file) — Python `CoreBridge` class that mmaps `/tmp/trt_lob_feed.bin`, validates the magic header, and exposes `read_new_ticks() -> list[dict]`. Track `last_seq`; on each call return only slots with seq in `[last_seq, write_seq)`, skipping overwritten slots. Convert each raw slot to a dict matching the existing pipeline schema (`timestamp_ns`, `exchange`, `symbol`, `best_bid`, `best_ask`, `bid_price_{1..5}`, `bid_size_{1..5}`, `ask_price_{1..5}`, `ask_size_{1..5}`). Return `[]` and skip silently if file absent or magic invalid.
  - acceptance: `CoreBridge().open()` returns `False` when C++ engine is not running (no crash)
  - acceptance: output dicts are schema-compatible with `pl.DataFrame(rows)` used throughout the pipeline
  - acceptance: ring-overflow slots (seq < write_seq - capacity) are silently dropped
- [x] **M8** `research/alpha/neural_alpha/pipeline.py` — Add `collect_from_core_bridge(n_ticks, interval_ms) -> pl.DataFrame | None` that polls `CoreBridge.read_new_ticks()` in a loop until `n_ticks` are accumulated. Update `collect_l5_ticks` to call `collect_from_core_bridge` first; fall back to REST only when the bridge is unavailable or returns fewer than `n_ticks // 2` rows. Training then receives combined BINANCE / KRAKEN / OKX / COINBASE ticks rather than a single REST endpoint. Depends on **M7**.
  - acceptance: `--synthetic` and `--data-path` flags still bypass all network calls
  - acceptance: REST fallback triggers and warns when bridge unavailable; no exception raised
  - acceptance: resulting DataFrame passes existing feature-engineering pipeline unchanged
- [x] **M9** `research/alpha/neural_alpha/shadow_session.py` — Replace the per-tick REST fetchers in `_fetch_tick` with a `CoreBridge` instance (opened in `__init__`, closed in session teardown). On each poll, call `bridge.read_new_ticks()` first; fall back to REST only when the bridge returns an empty list. Update `train_on_recent` to call `collect_from_core_bridge` so in-place retraining also uses live multi-exchange data. Depends on **M7**.
  - acceptance: bridge close called in `finally` block on session exit
  - acceptance: shadow IC and signal logs include ticks from all four exchanges when core engine is live
  - acceptance: REST fallback still functional as standalone mode (no core engine required)

### LOW
- [ ] **L1** `deploy/daily_train.py` — Add alerting webhook (Slack/PagerDuty) when model fails to promote or IC drops below floor (mail-only fallback is acceptable interim mitigation)

### RESEARCH
- [ ] **R1** — Research on integrate On-Chain metrics (Netflow, Spent Output Profit Ratio, Net Unrealised Profit/Loss, Whale transaction analysis, Defi Protocol metrics...)
- [x] **R2** — Research completed in `agents/reports/RESEARCH_R2_MARKET_REGIME_IDENTIFICATION_2026-03-14.md`: State-of-the-art real-time market regime identification and implementation blueprint for hybrid CPD + HMM/HSMM + microstructure overlays
- [ ] **R3** — Research on deep reinforcement learning for autonomous execution (State Space Design, Action Space design, Reward function formulation)
- [ ] **R4** — Research on hardware execution with Field-Programmable Gate Arrays and co-locating servers in the same clusters as exchanges
