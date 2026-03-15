## TODO List

### CRITICAL
- [ ] **C1** `core/execution/live_connector_base.hpp` + venue connectors — Replace generic auth/signature flow with exchange-spec canonical signing, remove non-cryptographic signature fallback in live path, and enforce hard-fail when cryptographic backend is unavailable
- [ ] **C2** `core/execution/*_connector.cpp` — Implement authenticated cancel/replace/query endpoints with strict response parsing; stop using synthetic venue order IDs and persist real exchange IDs
- [ ] **C3** `core/execution/` — Build full reconciliation service (open orders, fills, balances, positions) on reconnect + periodic drift checks with automated quarantine on mismatch
- [ ] **C4** `core/execution/` + `core/risk/` — Add durable idempotency journal and deterministic recovery for retry storms, duplicate acks, and cancel/replace race conditions

### HIGH
- [ ] **H1** `core/risk/` — Add portfolio/global risk controls (gross/net notional caps, concentration limits, venue caps, cross-venue netting limits) independent from strategy layer
- [ ] **H2** `core/shadow/shadow_engine.hpp` — Extend shadow simulator microstructure realism (queue position, partial fills, latency/slippage) and correct fee modeling for OKX/Coinbase
- [ ] **H3** `core/execution/smart_order_router.cpp` — Upgrade SOR objective to include fill probability, queue priority, adverse-selection/toxicity signals, and dynamic regime adaptation
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
