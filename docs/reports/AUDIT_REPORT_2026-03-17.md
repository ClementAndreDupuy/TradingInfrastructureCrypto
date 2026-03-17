# ThamesRiverTrading — Database & SLO Audit

**Audit date:** 2026-03-17  
**Scope:** End-to-end data storage layer (capture, persistence, retrieval, retention, observability) + production-grade SLO design for the project’s database plane.  
**Method:** Static repository audit of deployment, training, reporting, and infrastructure code.

---

## Executive Verdict (Direct)

The project does **not** currently operate a production-grade database platform. It operates a **file/object-store persistence pattern** (local Parquet + S3 artifacts) with limited governance and minimal database-quality reliability controls.

That is acceptable for R&D and early shadow, but **below industry standard** for a credible high/mid-frequency trading stack where data lineage, quality, latency, durability, and recoverability must be explicitly measured and enforced.

If this project wants external credibility, it needs to move from “files that exist” to “data platform with contractual reliability.”

---

## Current Database Reality (What Exists Today)

### 1) Persistence model is file-centric, not database-centric
- Training and cache data are stored as local Parquet files under `DATA_DIR`.
- Training outputs and metadata are emitted as JSON/Prometheus text files under `logs/`.
- Artifacts are uploaded to S3 in post-training hooks.

This is a **data exhaust pipeline**, not a controlled database service.

### 2) Infrastructure is single S3 bucket + lifecycle expiry
- Terraform provisions one artifacts bucket with versioning and a 90-day expiration rule.
- No explicit SSE-KMS policy, object lock, replication, storage class tiering, or immutability controls.

### 3) Observability exists for model/shadow metrics, not database SLIs
- Current Prometheus outputs focus on model quality and shadow PnL/fill metrics.
- No first-class SLIs for ingest lag, write success ratio, freshness, schema violations, or data completeness.

### 4) Documentation and runtime behavior are not fully aligned on data governance
- Deployment docs present multi-venue secrets flow and ops steps.
- Runtime scripts still reflect uneven maturity in symbol/exchange defaults and production-hardening assumptions.

---

## Gap vs Industry Standard (High/Mid-Frequency Trading)

## Critical gaps

1. **No canonical market-data store contract**  
   No single governed schema/versioned table design for ticks, book states, fills, and training datasets.

2. **No database SLO framework in operation**  
   There is no formal SLI/SLO/error-budget mechanism for data pipeline reliability.

3. **No data-quality gate in ingestion/training path**  
   Missing deterministic checks for null rates, monotonic sequence continuity, timestamp drift, duplicate rates, and venue completeness.

## High gaps

4. **Durability/recovery posture is shallow**  
   Versioning helps, but no tested RPO/RTO controls, restore drills, or region-level resilience strategy.

5. **Lineage and reproducibility are partial**  
   Model metadata exists, but dataset snapshot IDs/content hashes and full lineage joins are not enforced end-to-end.

6. **Retention policy is simplistic**  
   One fixed object expiration period does not match trading-data tiering needs (hot/warm/cold, compliance, research replay).

---

## State-of-the-Art SLO Plan for Database Plane

## SLO Architecture Principles

- **Service boundaries:** Separate SLIs for (a) ingest, (b) storage durability, (c) query/read serving, (d) data quality, (e) lineage.
- **Error-budget governance:** Weekly burn-rate alerts + release freeze when budget is exhausted.
- **Automated enforcement:** SLOs wired into CI/deploy gates and ops paging.
- **Dataset contracts:** Versioned schema + compatibility policy for every producer/consumer boundary.

## Proposed SLOs (v1 production set)

1. **Ingest Freshness SLO**  
   - **SLI:** `P99(event_time -> durable_store_time)` per venue/symbol.  
   - **Target:** ≤ 2s in shadow, ≤ 500ms in live.  
   - **Window:** 28 days.

2. **Write Availability SLO**  
   - **SLI:** successful durable writes / attempted writes.  
   - **Target:** 99.95% (shadow), 99.99% (live).

3. **Data Completeness SLO**  
   - **SLI:** expected sequence count vs persisted sequence count (after reconciliation window).  
   - **Target:** ≥ 99.99% completeness per day per venue.

4. **Data Correctness SLO**  
   - **SLI:** records passing schema + domain validation checks (price/size bounds, monotonicity, non-negative fields, timestamp sanity).  
   - **Target:** ≥ 99.999% valid rows.

5. **Query Latency SLO (Research/Backtest plane)**  
   - **SLI:** P95/P99 query runtime for standard feature-window retrieval jobs.  
   - **Target:** P95 < 3s, P99 < 10s for defined benchmark workloads.

6. **Durability & Recoverability SLO**  
   - **SLI:** verified backup/restore success + RPO/RTO conformance from drill runs.  
   - **Target:** 100% weekly restore drill pass; RPO ≤ 5 min, RTO ≤ 30 min.

7. **Lineage Coverage SLO**  
   - **SLI:** % production models with complete lineage tuple `{model_id, code_sha, dataset_snapshot_id, feature_schema_version}`.  
   - **Target:** 100%.

## Error budget policy

- 28-day window with automated burn-rate alerts:
  - **Fast burn:** 2h burn-rate > 14x budget ⇒ page immediately.
  - **Slow burn:** 24h burn-rate > 2x budget ⇒ ticket + on-call acknowledgement.
- Exhausted budget ⇒ freeze non-essential releases touching data pipelines until remediation complete.

## Instrumentation required

- Add DB SLI metric families:
  - `db_ingest_freshness_ms{venue,symbol}`
  - `db_write_success_total`, `db_write_failure_total`
  - `db_sequence_gap_total{venue,symbol}`
  - `db_schema_violation_total{field,rule}`
  - `db_restore_drill_success{env}`
  - `db_lineage_coverage_ratio`
- Emit daily SLO scorecard JSON + Prometheus gauges, consumed by Grafana/CloudWatch alarms.

## 30/60/90 day rollout

### Day 0–30 (Foundation)
- Define canonical schemas and versioning policy.
- Implement ingest/freshness/completeness counters.
- Stand up SLO dashboard + burn-rate alerts.

### Day 31–60 (Hardening)
- Add data-quality validators in pipeline gates.
- Implement dataset snapshot IDs + content hashing.
- Run weekly restore drills and publish RPO/RTO outcomes.

### Day 61–90 (State-of-the-art posture)
- Introduce table-format governance (Iceberg/Delta/Hudi) for ACID snapshots and schema evolution.
- Add multi-region data resilience path for critical datasets.
- Enforce release gates tied to SLO budget health.

---

## Immediate Actions (Credibility-Critical)

1. Make database SLOs a first-class production contract (not a TODO footnote).  
2. Add data-quality gates before training and before model promotion.  
3. Establish restore-drill discipline with measurable RPO/RTO.  
4. Move from raw file conventions to governed table semantics for canonical datasets.

Until these are in place, any “production-ready” claim remains weak under institutional due diligence.
