## TODO List

### CRITICAL
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
- [ ] **H1** `core/risk/` — Add portfolio/global risk controls (gross/net notional caps, concentration limits, venue caps, cross-venue netting limits) independent from strategy layer
- [x] **H2** `core/shadow/shadow_engine.hpp` — Extended shadow simulator realism with queue-position decay, partial fills, deterministic latency + slippage modeling, and venue-specific fee modeling for OKX/Coinbase; added dedicated `shadow_engine_test` unit coverage and build-script test wiring
- [x] **H3** `core/execution/smart_order_router.cpp` — Upgraded SOR objective to include fill probability, queue priority, adverse-selection/toxicity signals, and dynamic regime adaptation
- [ ] **H4** `deploy/` + monitoring stack — Define production SLOs/error budgets and wire hard alerts for feed integrity, reject spikes, reconciliation drift, and risk trigger frequency
- [ ] **H5** `tests/` + CI — Add sanitizer/race-test matrix (ASan/UBSan/TSan where applicable) and deterministic connector failure-injection suites

### MEDIUM
- [ ] **M1** `core/orderbook/orderbook.hpp` — Add dynamic/recentering grid strategy or adaptive bands to avoid prolonged out-of-grid degradation during volatile regimes
- [ ] **M2** `core/feeds/` + `tests/replay/` — Build feed-certification replay harness with venue-specific pathological scenarios and acceptance thresholds
- [ ] **M3** `research/alpha/neural_alpha/` + deploy — Add model governance: champion/challenger registry, automatic rollback policy, and drift-triggered safe mode

### LOW
- [ ] **L1** `deploy/daily_train.py` — Add alerting webhook (Slack/PagerDuty) when model fails to promote or IC drops below floor (mail-only fallback is acceptable interim mitigation)

### RESEARCH
- [ ] **R1** — Research on integrate On-Chain metrics (Netflow, Spent Output Profit Ratio, Net Unrealised Profit/Loss, Whale transaction analysis, Defi Protocol metrics...)
- [x] **R2** — Research completed in `agents/reports/RESEARCH_R2_MARKET_REGIME_IDENTIFICATION_2026-03-14.md`: State-of-the-art real-time market regime identification and implementation blueprint for hybrid CPD + HMM/HSMM + microstructure overlays
- [ ] **R3** — Research on deep reinforcement learning for autonomous execution (State Space Design, Action Space design, Reward function formulation)
- [ ] **R4** — Research on hardware execution with Field-Programmable Gate Arrays and co-locating servers in the same clusters as exchanges
