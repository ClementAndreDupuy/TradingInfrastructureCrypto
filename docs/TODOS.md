## TODO List

Open items carried forward from the previous TODO cycle, ranked by priority/severity.

### CRITICAL
- _No open critical items._

### HIGH
- [ ] **H4** `deploy/` + monitoring stack — Define production SLOs/error budgets and wire hard alerts for feed integrity, reject spikes, reconciliation drift, and risk trigger frequency
- [ ] **H5** `research/neural_alpha/` + `deploy/` — Productionize secondary-model ensemble rollout: add canary guardrails, live IC/ICIR degradation rollback thresholds, and publish promotion/rollback events to ops alerts
- [ ] **H6** `deploy/` + `research/` + `docs/` — Establish a formal database SLO framework (ingest freshness, write availability, completeness, correctness, durability, lineage) with 28-day error budgets and burn-rate paging
  - acceptance criteria:
    - [ ] Published SLO spec in `docs/` defining SLIs, targets, windows, and owners for freshness, availability, completeness, correctness, durability, and lineage
    - [ ] Prometheus/CloudWatch metrics emitted for each SLI with Grafana dashboard panels and burn-rate alerts (fast + slow) wired to on-call
    - [ ] 28-day error-budget policy enforced in operations runbook with explicit freeze/escalation rules after budget exhaustion
- [ ] **H7** `deploy/aws/terraform/` — Harden data durability/security posture: enforce SSE-KMS on S3 artifacts bucket, add immutable retention controls for critical datasets, and implement tested backup/restore drills with explicit RPO/RTO targets
  - acceptance criteria:
    - [ ] Terraform enforces SSE-KMS and blocks unencrypted object uploads for artifacts paths
    - [ ] Object retention/immutability controls enabled for critical datasets with documented exceptions
    - [ ] Backup/restore drill automation added and executed successfully with recorded RPO/RTO results meeting targets

### MEDIUM
- [ ] **M2** `core/feeds/` + `tests/replay/` — Build feed-certification replay harness with venue-specific pathological scenarios and acceptance thresholds
- [ ] **M3** `research/neural_alpha/pipeline.py` — Enforce live-data-only training in deployment jobs (no synthetic dataset execution paths in prod runtime wrappers)
- [ ] **M4** `research/neural_alpha/` + `tests/integration/` — Add live-capture integration validation for ensemble checkpoints against core IPC feed snapshots (no mocked data path)
- [ ] **M5** `research/neural_alpha/` + `deploy/daily_train.py` — Add strict data-quality gates (schema validation, null-rate bounds, sequence-gap checks, timestamp skew limits, duplicate detection) before training and model promotion
  - acceptance criteria:
    - [ ] Training job fails closed when any quality gate breaches configured thresholds
    - [ ] Quality report artifact (JSON + log summary) emitted per run with breach reasons and affected venues/symbols
    - [ ] Model promotion path blocked unless latest dataset passes all quality gates
- [ ] **M6** `deploy/` + `docs/` — Define canonical dataset contracts and lineage (`dataset_snapshot_id`, schema version, content hash, producer code SHA) and enforce them in training metadata/artifact promotion
  - acceptance criteria:
    - [ ] Canonical schema contract document published with versioning/compatibility rules
    - [ ] Training metadata includes dataset snapshot ID, schema version, content hash, and producer code SHA
    - [ ] Promotion tooling rejects artifacts with missing or invalid lineage fields

### LOW
- [ ] **L1** `deploy/daily_train.py` — Add alerting webhook (Slack/PagerDuty) when model fails to promote or IC drops below floor (mail-only fallback is acceptable interim mitigation)

### RESEARCH
- [ ] **R1** — Research on integrate On-Chain metrics (Netflow, Spent Output Profit Ratio, Net Unrealised Profit/Loss, Whale transaction analysis, Defi Protocol metrics...)
- [ ] **R3** — Research on deep reinforcement learning for autonomous execution (State Space Design, Action Space design, Reward function formulation)
- [ ] **R4** — Research on hardware execution with Field-Programmable Gate Arrays and co-locating servers in the same clusters as exchanges
