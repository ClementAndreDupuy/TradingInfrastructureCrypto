# Production SLO Framework

This document defines the production SLO/SLI contract for H4 and H6.

## Scope

- Trading-plane operational reliability (feed integrity, rejects, reconciliation, risk triggers).
- Database-plane reliability (freshness, write availability, completeness, correctness, durability, lineage).
- Window: rolling 28 days for all budgets.

## Ownership

- Trading SLO owner: Core execution on-call (primary) + feeds on-call (secondary).
- Database SLO owner: Research/data platform on-call (primary) + deployment on-call (secondary).
- Escalation: incident commander (IC) and head of trading systems after budget exhaustion.

## Trading SLOs (H4)

| SLI | Metric | Target (28d) | Error budget |
|---|---|---:|---:|
| Feed integrity | `trading_sli_feed_integrity` | 99.90% | 0.10% |
| Reject health | `trading_sli_reject_health` | 99.50% | 0.50% |
| Reconciliation health | `trading_sli_reconciliation_health` | 99.90% | 0.10% |
| Risk trigger health | `trading_sli_risk_health` | 99.95% | 0.05% |

Definitions:
- Feed integrity = share of feed samples with `staleness_ms <= 500` and `sequence_gaps == 0`.
- Reject health = share of terminal orders not rejected.
- Reconciliation health = share of reconciliation cycles with `drift_ok=true`.
- Risk trigger health = share of risk checks without circuit-breaker/kill-switch trigger.

## Database SLOs (H6)

| SLI | Metric | Target (28d) | Error budget |
|---|---|---:|---:|
| Ingest freshness | `db_sli_ingest_freshness` | 99.90% | 0.10% |
| Write availability | `db_sli_write_availability` | 99.95% | 0.05% |
| Completeness | `db_sli_completeness` | 99.90% | 0.10% |
| Correctness | `db_sli_correctness` | 99.99% | 0.01% |
| Durability | `db_sli_durability` | 99.99% | 0.01% |
| Lineage coverage | `db_sli_lineage` | 99.90% | 0.10% |

Definitions:
- Ingest freshness: event persisted within 30 seconds of capture timestamp.
- Write availability: DB write operations complete successfully.
- Completeness: `actual_records / expected_records >= 99.9%`.
- Correctness: checksum/hash validation succeeds.
- Durability: persisted data survives durability checks and backup verification.
- Lineage: dataset has valid `dataset_snapshot_id`, schema version, content hash, producer SHA.

## Burn-rate paging policy

All SLOs page using multi-window burn-rate alerts:
- Fast burn: 2h burn rate > 14x budget => page on-call immediately.
- Slow burn: 24h burn rate > 2x budget => create incident ticket + ack by on-call.

Burn-rate reference formula:
- `burn_rate = bad_events / allowed_bad_events` over aligned window.

## Instrumentation contract

- Metric generation job: `python deploy/slo_metrics.py`.
- Prometheus textfile output: `logs/slo_metrics.prom`.
- Daily scorecard artifact: `logs/slo_scorecard_<yyyymmdd>.json`.
- Dashboard and alert rules: `deploy/monitoring/grafana_slo_dashboard.json` and `deploy/monitoring/prometheus_slo_rules.yml`.
- CloudWatch forwarding: ship `slo_metrics.prom` gauges into CloudWatch namespace `Trading/SLO`.
