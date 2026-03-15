# ThamesRiverTrading — Pre-Production Audit (Deep Dive)

**Date:** 2026-03-15  
**Scope:** Full-stack review (C++ hot path, exchange feeds/connectors, risk, execution, shadow, Python research/backtest, deployment).  
**Benchmark:** What serious high/mid-frequency shops (e.g., HRT/Jump-style engineering standards) typically require for production sign-off.

---

## 1) Brutally Honest Executive Verdict

This codebase is **meaningfully better than a toy prototype** and has strong building blocks, but it is **still not ready for unrestricted production capital**.

If I compare this to institutional-grade deployment standards, current status is:

- **Architecture quality:** good
- **Research pipeline quality:** decent for iteration speed
- **Market data engineering:** decent with important caveats
- **Execution safety/completeness:** below institutional bar
- **Operational hardening / controls:** below institutional bar
- **Production readiness overall:** **NO-GO** for serious live risk, **GO** for extended shadow / low-notional staged rollout only

In plain terms: this is a **promising pre-production system**, not a mature production trading plant.

---

## 2) Scorecard vs Institutional Standard (HRT/Jump-Style Expectations)

| Domain | Current State | Institutional Bar | Gap |
|---|---|---|---|
| Feed handlers | Multi-venue handlers with reconnect/snapshot/delta logic | Venue-spec exactness + deterministic failover + replay certification | **Medium** |
| Order book | Fast flat-array approach, perf test coverage | Deterministic correctness under extreme edge cases + formal replay gates | **Medium** |
| Execution connectors | Implemented for 4 venues but thin REST wrappers | Full auth correctness, ack/reject parsing, partial fills, websocket user streams, reconciler | **Critical** |
| Risk controls | Kill switch + circuit breaker checks exist | Portfolio-level limits, venue-level throttles, exposure laddering, auto-quarantine, tested incident runbooks | **High** |
| Smart routing | Basic scorer and alpha gate | Queue-position/impact-aware router with probabilistic fill models and dynamic toxicity controls | **High** |
| Shadow parity | Same interface path, useful for validation | Fill simulator calibrated to venue microstructure and fees for all venues | **High** |
| Observability | Logging + some tests + systemd scripts | Structured metrics, SLO/error budgets, incident dashboards, alerting with clear ownership | **High** |
| SDLC quality gates | Unit/replay/perf tests exist | Deterministic CI with sanitizer matrix, race checks, perf regression gates on dedicated runners | **High** |
| Security/compliance posture | Env-based secrets and placeholders | Key rotation policy, signed artifacts, immutable deploys, audit trails, strict prod credential handling | **High** |

---

## 3) Deep Findings by Component

## A. Execution Connectors (Critical)

### What is good
- There is now a real connector abstraction and per-venue implementations.
- Idempotency key concept and retry policy are present.
- A `trading_engine` executable path now exists.

### What is not good enough
1. **Auth/signing is not exchange-spec compliant.**
   - `LiveConnectorBase::auth_headers` applies generic headers (`X-API-KEY`, `X-TS`, `X-SIGN`) to all venues, but each exchange requires distinct canonical signing formats and headers.
   - A non-OpenSSL fallback uses `std::hash` as signature material, which is not cryptographic and not acceptable for live trading security.

2. **Cancel endpoints often omit authentication headers.**
   - Venue cancel methods call `http::post`/`http::del` without passing auth headers in several connectors.
   - That means “works in mock mode” but likely fails on real venues.

3. **Venue order IDs are mostly synthetic placeholders.**
   - `venue_order_id` is derived from `client_order_id` with static prefixes instead of parsing exchange responses.
   - This breaks true reconciliation and incident forensics.

4. **No proper live state reconciliation.**
   - `reconcile()` currently clears maps; it does not compare open orders/fills/positions with venue truth.

5. **No private websocket/user-stream integration.**
   - Institutional setups consume private streams for order/fill latency and state consistency.

**Bottom line:** connector layer has shape, but not production substance yet.

---

## B. Risk Engine & Guardrails (High)

### What is good
- Circuit breaker includes rate, drawdown, stale book, deviation, consecutive loss checks.
- Kill switch path is in place.
- Market maker pre-submit checks call circuit breaker checks.

### Gaps
1. **Risk mostly strategy-local, not portfolio/global.**
   - Missing account-level exposure caps, cross-venue netting controls, per-asset gross/notional concentration limits.

2. **No explicit hard risk ownership pipeline.**
   - Institutional desks have independent risk process: pre-open checks, intraday guardrail tuning, post-incident lockouts.

3. **No persistent risk event ledger.**
   - Logs exist, but no append-only audited risk-event store for postmortem/compliance.

---

## C. Feed Handlers & Market Data Integrity (Medium)

### What is good
- Handlers exist for Binance/Kraken/OKX/Coinbase.
- Snapshot + buffering + sequence checks + reconnect logic are present.
- OKX checksum path is implemented.

### Gaps
1. **Coinbase handling is L2-focused only.**
   - Adequate for many crypto strategies, but below bar for strategies that need richer order lifecycle granularity.

2. **No explicit feed certification harness documented.**
   - HRT/Jump-style process includes per-venue certification suites and deterministic packet-replay acceptance criteria.

3. **Operational visibility on feed quality is thin.**
   - Missing first-class metrics for gap frequency, resnapshot rates, sequence anomalies, and reconnect MTTR.

---

## D. Order Book / Core Data Structures (Medium)

### What is good
- Flat-array book is simple, fast, and cache-friendly.
- Sequence and checksum checks exist.
- Performance test exists for microsecond-level target.

### Gaps
1. **Assumptions around static grid centering may degrade under regime shifts.**
   - Large fast moves can push prices out-of-grid, causing skipped levels and data quality degradation.

2. **No formal verification for extreme edge cases.**
   - Institutional systems maintain aggressive replay corpora for pathological conditions.

---

## E. Smart Order Routing (High)

### What is good
- Cost-based venue scoring and alpha gating exist.
- Child order split logic is straightforward and testable.

### Gaps
1. **Routing objective is simplistic.**
   - Uses linear score from quote + fee + latency/risk penalties.
   - No queue-position, fill probability, adverse selection model, or dynamic microstructure regime adaptation.

2. **No explicit anti-toxicity / toxicity throttling loop.**
   - Advanced shops continuously estimate toxicity and adapt routing aggression.

---

## F. Shadow Trading Parity (High)

### What is good
- Shadow connector reuses live interfaces and logs decisions.
- Good step toward safe rollout culture.

### Gaps
1. **Fill model too optimistic/simplistic for institutional calibration.**
   - Crossing best bid/ask logic is not enough for realistic slippage/queue-position outcomes.

2. **Fee model incomplete for all venues.**
   - `ShadowConfig` is Binance/Kraken-centric; other venues fall through generic logic.

3. **No mandatory shadow promotion gate codified in CI/CD.**
   - Need strict promotion policy tied to objective KPIs over minimum sample horizon.

---

## G. Research / Backtest / MLOps (Medium–High)

### What is good
- End-to-end neural alpha pipeline exists.
- Synthetic mode and unit tests are present.
- Separation of cold path vs hot path is correct.

### Gaps
1. **Backtest realism and production tracking not yet institutional depth.**
   - Need stronger transaction cost/slippage modeling, venue-specific fill assumptions, drift detection, and formal champion/challenger governance.

2. **Model risk controls under-specified.**
   - Need formal model version lineage, rollback policy, and “safe mode” behavior when signal quality degrades.

---

## H. Deployment / SRE / Incident Preparedness (High)

### What is good
- Terraform + systemd + scripts provide operational skeleton.
- Service definitions and environment templates exist.

### Gaps
1. **Not yet an immutable release pipeline.**
   - Need signed artifacts, reproducible build provenance, strict release promotion workflow.

2. **Incident runbooks are not complete enough for 24/7 trading ops.**
   - Need codified playbooks for venue outage, stale data storms, auth failure bursts, and reconciliation divergence.

3. **Alerting and SLOs are still weak.**
   - Need error budgets and hard paging criteria (latency, reject spikes, feed integrity, risk trigger rate).

---

## 4) Where You Already Compete Well

- Clean architecture split (hot path C++ / cold path Python).
- Clear intent for multi-venue, unified abstractions.
- Presence of risk primitives and replay/perf tests.
- Practical shadow infrastructure and deployment scaffolding.

This is a **solid foundation**. The missing part is **institutional hardening**, not starting-from-zero engineering.

---

## 5) Top Gaps vs HRT/Jump-Like Standard (Prioritized)

## CRITICAL (must close before real production capital)
1. Exchange-spec-correct authentication/signing and request canonicalization per venue.
2. Real private order-state streams + robust reconciliation (orders/fills/positions/balances).
3. Connector correctness under failure: retries, idempotency persistence, duplicate-ack handling, cancel/replace races.
4. Deterministic operational kill paths with audited event trail.

## HIGH
1. Portfolio/global risk engine and hard limits independent from strategy logic.
2. Advanced SOR with probabilistic fill/adverse-selection control.
3. Realistic shadow simulator calibration and promotion gating policy.
4. Production observability: metrics, dashboards, alerts, incident ownership model.

## MEDIUM
1. Feed certification harness and expanded replay corpus.
2. Book/grid resilience improvements for extreme moves.
3. MLOps governance: model lineage, rollback triggers, drift controls.

---

## 6) Final Go/No-Go Decision

- **Live with meaningful capital:** **NO-GO**
- **Extended shadow / very small notional canary with hard kill-switch and manual supervision:** **conditional GO**

If you close critical execution/auth/reconciliation gaps and implement high-priority risk+ops controls, this can become production-grade. Right now, it is **close structurally but not safe enough operationally**.
