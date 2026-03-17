# SLO Operations Runbook

## Inputs

- SLO scorecard: `logs/slo_scorecard_<yyyymmdd>.json`
- Prometheus metrics: `logs/slo_metrics.prom`
- Alert rules: `deploy/monitoring/prometheus_slo_rules.yml`

## Alert routing

- Critical pages: PagerDuty service `trading-prod-oncall`.
- Non-critical tickets: Ops queue `TRADING-SLO`.

## 28-day error-budget enforcement policy

1. **Healthy budget** (`error_budget_remaining >= 50%`)  
   Continue normal release cadence.
2. **Warning budget** (`20% <= remaining < 50%`)  
   Require on-call acknowledgement and mitigation task in sprint backlog.
3. **Critical budget** (`0% < remaining < 20%`)  
   Freeze non-essential deploys touching affected SLI owner domain until trend improves for 3 consecutive days.
4. **Exhausted budget** (`remaining == 0%`)  
   - Immediate release freeze on affected domain.
   - Incident commander assigned within 15 minutes.
   - Root-cause and remediation plan due within 24h.
   - Unfreeze requires owner + IC approval and one full day without further budget burn breach.

## Escalation matrix

- Trading SLI breach: core execution lead -> trading systems head.
- Database SLI breach: research/data platform lead -> trading systems head.
- Cross-domain breach: incident commander leads a joint bridge with both owners.

## Manual verification commands

```bash
python deploy/slo_metrics.py
cat logs/slo_metrics.prom
```
