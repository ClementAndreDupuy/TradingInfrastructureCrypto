## TODO List

Open items carried forward from the previous TODO cycle, ranked by priority/severity.

### CRITICAL
- _No open critical items._

### HIGH
- [ ] **H4** `deploy/` + monitoring stack — Define production SLOs/error budgets and wire hard alerts for feed integrity, reject spikes, reconciliation drift, and risk trigger frequency
- [ ] **H5** `research/neural_alpha/` + `deploy/` — Productionize secondary-model ensemble rollout: add canary guardrails, live IC/ICIR degradation rollback thresholds, and publish promotion/rollback events to ops alerts

### MEDIUM
- [ ] **M2** `core/feeds/` + `tests/replay/` — Build feed-certification replay harness with venue-specific pathological scenarios and acceptance thresholds
- [ ] **M3** `research/neural_alpha/pipeline.py` — Enforce live-data-only training in deployment jobs (no synthetic dataset execution paths in prod runtime wrappers)
- [ ] **M4** `research/neural_alpha/` + `tests/integration/` — Add live-capture integration validation for ensemble checkpoints against core IPC feed snapshots (no mocked data path)

### LOW
- [ ] **L1** `deploy/daily_train.py` — Add alerting webhook (Slack/PagerDuty) when model fails to promote or IC drops below floor (mail-only fallback is acceptable interim mitigation)

### RESEARCH
- [ ] **R1** — Research on integrate On-Chain metrics (Netflow, Spent Output Profit Ratio, Net Unrealised Profit/Loss, Whale transaction analysis, Defi Protocol metrics...)
- [ ] **R3** — Research on deep reinforcement learning for autonomous execution (State Space Design, Action Space design, Reward function formulation)
- [ ] **R4** — Research on hardware execution with Field-Programmable Gate Arrays and co-locating servers in the same clusters as exchanges
