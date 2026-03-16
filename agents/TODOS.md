## TODO List

### CRITICAL
- [x] **C-NEW-1** `core/engine/trading_engine_main.cpp` — Replace demo harness with a production event-driven daemon: construct feed handlers + book managers for all 4 venues, run continuous WebSocket event loop, call `NeuralAlphaMarketMaker::on_book_update()` on every tick, periodic `ReconciliationService` call every 30 s, heartbeat keepalive for kill switch, clean SIGTERM/SIGINT shutdown handler. Current binary fires one SOR decision and exits — SystemD restart loop would submit uncoordinated sporadic orders with no position management.
  - acceptance: binary runs continuously until SIGTERM; does not exit on clean operation
  - acceptance: kill switch heartbeat is refreshed at ≥ 1 Hz
  - acceptance: reconciliation runs on reconnect and every 30 s
- [ ] **C-NEW-2** `deploy/systemd/neural-alpha-shadow.service` — Fix `--exchanges SOLANA` to `--exchanges BINANCE` (or BINANCE,KRAKEN). Shadow session currently runs against a non-existent exchange; all shadow validation data (IC, fill rate) has been collected on the wrong instrument/venue. Shadow must match intended live venues before go/no-go evaluation is meaningful.
- [ ] **C-NEW-3** `deploy/aws/userdata.sh` — Bootstrap script only fetches and writes Binance credentials; Kraken/OKX/Coinbase `API_KEY` + `API_SECRET` are missing from `/etc/trading/env`. `LiveConnectorBase::connect()` returns `AUTH_FAILED` for 3 of 4 connectors at startup. Fix: load all 4 exchange key-pairs from Secrets Manager into the env file.
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

### LOW
- [ ] **L1** `deploy/daily_train.py` — Add alerting webhook (Slack/PagerDuty) when model fails to promote or IC drops below floor (mail-only fallback is acceptable interim mitigation)

### RESEARCH
- [ ] **R1** — Research on integrate On-Chain metrics (Netflow, Spent Output Profit Ratio, Net Unrealised Profit/Loss, Whale transaction analysis, Defi Protocol metrics...)
- [x] **R2** — Research completed in `agents/reports/RESEARCH_R2_MARKET_REGIME_IDENTIFICATION_2026-03-14.md`: State-of-the-art real-time market regime identification and implementation blueprint for hybrid CPD + HMM/HSMM + microstructure overlays
- [ ] **R3** — Research on deep reinforcement learning for autonomous execution (State Space Design, Action Space design, Reward function formulation)
- [ ] **R4** — Research on hardware execution with Field-Programmable Gate Arrays and co-locating servers in the same clusters as exchanges
