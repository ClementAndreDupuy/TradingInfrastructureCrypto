## TODO List

Open items carried forward from the previous TODO cycle, ranked by priority/severity.
Last updated: 2026-03-18 — post build/shadow-run audit.

---

### CRITICAL

- [ ] **C1** `deploy/scripts/build.sh` — Python pip install fails with PEP 668 `externally-managed-environment` when `python3.12` is selected as the interpreter. Both `pip install -e .` and the standard-install fallback exit non-zero; bindings build is subsequently skipped. Fix: pass `--break-system-packages` to both pip calls in `action_python_install()`, or have the build script create and activate a virtualenv before the install step.
  - acceptance criteria:
    - [ ] `./deploy/scripts/build.sh --skip-cpp --skip-tests` completes without Python install warnings on a clean Ubuntu 24.04 host with no pre-existing venv
    - [ ] pybind11 bindings step (`action_bindings`) runs after a successful Python install

- [ ] **C2** `core/` (CMake) — C++ build blocked by three missing system packages: `libwebsockets-dev`, `libcurl4-openssl-dev`, `nlohmann-json3-dev`. CMake configure fails at `pkg_check_modules(LIBWEBSOCKETS REQUIRED libwebsockets)` in `core/CMakeLists.txt`. The preflight check correctly detects all three but the build has no automated bootstrap path.
  - acceptance criteria:
    - [ ] `deploy/scripts/build.sh` (or a companion bootstrap script) installs the three packages when absent and the caller has `sudo` / root access
    - [ ] `build/bin/trading_engine` exists after a clean build on Ubuntu 24.04 with standard dev toolchain

---

### HIGH

- [ ] **H1** `deploy/run_shadow.sh` + `research/neural_alpha/pipeline.py` — Binance Futures REST endpoint (`fapi.binance.com`) returns HTTP 451 (geographic/regulatory restriction) in the current build environment. All Binance L5 tick fetches silently fail; bootstrap training cannot collect enough ticks from Binance alone. Kraken, OKX, and Coinbase fetch paths all succeed.
  - acceptance criteria:
    - [ ] Shadow session detects 451 for Binance and skips it gracefully (no retry storm), redistributing tick collection across the remaining three venues
    - [ ] `run_shadow.sh` documents (via log line or `--help`) that Binance Futures is region-restricted and falls back to alternate venues
    - [ ] Bootstrap training succeeds when only three of the four venues are reachable

- [ ] **H2** `deploy/` + `research/` + `docs/` — Extend SLO engine with ingest-freshness and write-availability SLIs; add 28-day error-budget burn-rate alerting once a Prometheus/Grafana stack is available *(carried from H6)*
  - acceptance criteria:
    - [ ] Ingest freshness (P99 event→store latency) and write availability counters added to `slo_engine.py`
    - [ ] Burn-rate alert rules (fast: 2 h × 14× budget; slow: 24 h × 2× budget) configured in Prometheus/CloudWatch
    - [ ] 28-day error-budget policy documented in ops runbook with freeze/escalation rules

- [ ] **H3** `deploy/aws/terraform/` — Harden data durability/security posture: enforce SSE-KMS on S3 artifacts bucket, add immutable retention controls for critical datasets, and implement tested backup/restore drills with explicit RPO/RTO targets *(carried from H7)*
  - acceptance criteria:
    - [ ] Terraform enforces SSE-KMS and blocks unencrypted object uploads for artifacts paths
    - [ ] Object retention/immutability controls enabled for critical datasets with documented exceptions
    - [ ] Backup/restore drill automation added and executed successfully with recorded RPO/RTO results meeting targets

---

### MEDIUM

- [ ] **M1** `core/feeds/` + `tests/replay/` — Build feed-certification replay harness with venue-specific pathological scenarios and acceptance thresholds *(carried from M2)*

- [ ] **M2** `deploy/` + `docs/` — Define canonical dataset contracts and lineage (`dataset_snapshot_id`, schema version, content hash, producer code SHA) and enforce them in training metadata/artifact promotion *(carried from M6)*
  - acceptance criteria:
    - [ ] Canonical schema contract document published with versioning/compatibility rules
    - [ ] Training metadata includes dataset snapshot ID, schema version, content hash, and producer code SHA
    - [ ] Promotion tooling rejects artifacts with missing or invalid lineage fields

- [ ] **M3** `research/neural_alpha/` + `deploy/daily_train.py` — Data-quality gates partially implemented; acceptance items still open *(carried from M5)*
  - acceptance criteria:
    - [ ] Training job fails closed when any quality gate breaches configured thresholds
    - [ ] Quality report artifact (JSON + log summary) emitted per run with breach reasons and affected venues/symbols
    - [ ] Model promotion path blocked unless latest dataset passes all quality gates

---

### LOW

- [ ] **L1** `deploy/daily_train.py` — Add alerting webhook (Slack/PagerDuty) when model fails to promote or IC drops below floor (mail-only fallback is acceptable interim mitigation)

---

### RESEARCH

- [ ] **R1** — Research on integrating on-chain metrics (Netflow, Spent Output Profit Ratio, Net Unrealised Profit/Loss, Whale transaction analysis, DeFi Protocol metrics)
- [ ] **R3** — Research on deep reinforcement learning for autonomous execution (State Space Design, Action Space design, Reward function formulation)
- [ ] **R4** — Research on hardware execution with Field-Programmable Gate Arrays and co-locating servers in the same clusters as exchanges
