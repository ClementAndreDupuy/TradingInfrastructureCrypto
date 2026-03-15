# ThamesRiverTrading Production Readiness Audit

**Audit date:** 2026-03-14
**Scope:** Full codebase audit against industry-standard high/mid-frequency crypto production requirements, with explicit focus on Binance/OKX/Kraken/Coinbase feed + order readiness.

---

## Executive Verdict (No Sugarcoating)

This repository is **not production ready**.

You have a strong architecture and meaningful building blocks (feed handlers, order book, shadow connector, risk primitives, model pipeline), but the core production path is incomplete:

- **No live exchange order connectors exist** (only `ShadowConnector`), so the system cannot place/cancel/reconcile real orders on Binance/OKX/Kraken/Coinbase.
- Deployment expects a `trading_engine` binary that does not exist in the repository build graph.
- Feed handlers exist for all 4 exchanges, but **spec compliance is incomplete** (notably Coinbase protocol depth and OKX checksum controls).
- Environment/bootstrap reproducibility for C++ is fragile due to required native dependencies and no local fallback.

Industry-standard firms would classify this as **pre-production prototype / shadow-only**.

---

## Exchange Readiness Matrix (Orders, Feeds, Books)

| Exchange | Feed Handler | Order Book Integration | Live Order Routing | Verdict |
|---|---|---|---|---|
| Binance | Implemented WebSocket + snapshot sync | Integrated via `BookManager`/`OrderBook` callbacks | **Missing** live connector | **Not production ready** |
| Kraken | Implemented WebSocket + snapshot sync | Integrated via `BookManager`/`OrderBook` callbacks | **Missing** live connector | **Not production ready** |
| OKX | Implemented WebSocket + snapshot sync | Integrated via `BookManager`/`OrderBook` callbacks | **Missing** live connector | **Not production ready** |
| Coinbase | Implemented L2 WebSocket feed | Integrated via `BookManager`/`OrderBook` callbacks | **Missing** live connector | **Not production ready** |

### Key conclusion
You currently have **market data ingestion for 4 exchanges**, but **zero live execution connectors for all 4**. This is the largest production gap.

---

## Findings by Severity

## CRITICAL (Blocks Production)

1. **No live order connectivity layer implemented.**
   `ExchangeConnector` is an interface and only `ShadowConnector` implements it. No Binance/Kraken/OKX/Coinbase REST/FIX/WebSocket trading connectors, no authenticated order submit/cancel path, no reconciliation implementation against live venues.

2. **Deployment references non-existent C++ trading engine binary.**
   Systemd service starts `/opt/trading/build/bin/trading_engine`, but no target or `main()` for this binary is present in CMake/repo.

3. **Production run scripts are inconsistent with stated 4-exchange objective.**
   Live/shadow scripts are Binance-keyed and launch Python `shadow_session` with `--exchanges "SOLANA"`, not Binance/Kraken/OKX/Coinbase execution orchestration.

4. **Local C++ build is non-portable by default due to hard dependency on `libwebsockets` dev package.**
   Build fails at configure stage when the package is missing; this impairs reliability of developer onboarding and emergency patch workflows.

---

## HIGH (Major Industry Gaps)

1. **Coinbase handler does not meet internal exchange standard listed in project guidance.**
   Guidance says Coinbase should be full L3/FIX style; implementation is `level2`/`l2_data` book updates only.

2. **OKX checksum validation is not implemented.**
   Internal guidance explicitly marks CRC32 checksum as required for `books`; code validates sequence continuity but does not verify checksum integrity.

3. **Risk controls not fully wired into strategy decision loop.**
   `NeuralAlphaMarketMaker` gates on kill-switch and local signal risk score widening, but does not directly apply `CircuitBreaker` checks before order submissions.

4. **Unit feed tests are partially integration-like and may depend on live network behavior.**
   Some tests call handler `start()` and expect sequencing behavior, increasing flakiness and reducing deterministic CI confidence.

---

## MEDIUM (Quality / Operations / Reliability)

1. **No explicit latency budget enforcement tests in CI.**
   Budgets are documented (<1µs orderbook/risk, <10µs feed handler), but no benchmark gate fails builds on regression.

2. **Production readiness docs and implementation are out of sync.**
   Deployment narrative implies a C++ live engine while operational scripts currently run Python shadow/live sessions.

3. **Cross-exchange execution abstraction is incomplete.**
   `ShadowEngine` currently instantiated for Binance/Kraken; no equivalent production-grade multi-venue router for OKX/Coinbase.

4. **Dependency bootstrap is split across CI and local instructions.**
   CI installs required native packages explicitly; local build instructions do not provide a robust fallback or preflight checker.

---

## Industry Standard Comparison (HFT / Mid-Frequency Crypto)

Compared to production desks, this repo is currently:

- **Good:** architecture split, order book design, risk primitive components, shadow-mode concept, model workflow.
- **Behind standard:** authenticated exchange execution stack, protocol fidelity (Coinbase depth mode / OKX checksum), deterministic runbooks, operationally verified live engine path.
- **Missing for production sign-off:** exchange certification checklist, chaos/reconnect drills, kill-switch tabletop + live drills, production SLO/error budgets, rollback playbooks tied to verified binary artifacts.

---

## Production Go/No-Go Decision

**Decision: NO-GO for production live trading.**

Reason: no live order connectors, no verifiable trading engine binary, exchange protocol compliance gaps, and incomplete risk wiring for pre-trade guards.

---

## Recommended Remediation Sequence (Pragmatic)

1. **Implement live connectors (Binance/Kraken/OKX/Coinbase)** with authenticated submit/cancel/replace/reconcile and deterministic idempotency.
2. **Create and ship real `trading_engine` binary** target with startup health checks and connector wiring.
3. **Close exchange protocol gaps** (OKX CRC32 checksum, Coinbase depth/protocol choice aligned with your stated standard).
4. **Wire `CircuitBreaker` directly into order placement path** (hard gate before `submit_order`).
5. **Harden CI/test matrix** with deterministic feed replay, connector contract tests, and latency regression gates.
6. **Align deployment scripts/docs** with actual four-exchange execution topology.

