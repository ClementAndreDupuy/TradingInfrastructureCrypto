# ThamesRiverTrading — Production Readiness Audit

**Audit date:** 2026-03-16
**Scope:** Full codebase audit focused on shadow-mode deployment readiness and remaining gaps vs industry standard mid-freq crypto trading.
**Prior audits:** 2026-03-12, 2026-03-14, 2026-03-15

---

## Executive Verdict (No Sugarcoating)

**The system is NOT ready for shadow-mode deployment in its current state.**

The individual C++ components (order book, feed handlers, connectors, reconciliation, risk) are solid and have improved significantly since the March 14 audit. However, three critical integration failures prevent safe shadow deployment:

1. **`trading_engine_main.cpp` is a demo harness, not a production daemon.** It fires one SOR decision and exits — the binary cannot run a market-making session.
2. **The shadow session targets `SOLANA`** — a non-existent exchange in this system — so no shadow validation against the intended 4 venues has ever run.
3. **`userdata.sh` only injects Binance credentials**, leaving Kraken/OKX/Coinbase API keys absent from the deployed environment.

These are not subtle issues. They mean the system as deployed cannot trade, cannot shadow-trade on the right exchanges, and would fail to authenticate 3 of 4 connectors at boot.

---

## What Has Improved Since March 14

| Gap (March 14 Audit) | Status |
|---|---|
| No live order connectors (CRITICAL) | ✅ **Fixed** — all 4 exchange connectors implemented with real auth, submit/cancel/replace/query |
| No `trading_engine` binary target | ✅ **Fixed** — target exists in CMake and `trading_engine_main.cpp` compiles |
| OKX checksum validation missing | ✅ **Fixed** — `crc32_bytes` + `validate_checksum` implemented in `okx_feed_handler.cpp` |
| Risk controls not wired into order path | ✅ **Fixed** — `CircuitBreaker` + `GlobalRiskControls` are hard-gated in `trading_engine_main.cpp` |
| Reconciliation service: stubs only | ✅ **Substantially fixed** — staged remediation, canonical diff, fill deduplication, incident trail |
| Idempotency journal | ✅ **Fixed** — durable on-disk journal with replay and state machine |
| Shadow engine realism | ✅ **Fixed** — queue-position decay, partial fills, per-venue fee modeling |
| Smart order router | ✅ **Fixed** — toxicity/fill-probability scoring, regime adaptation |

---

## Critical Issues (Block Shadow-Mode Deployment)

### C-NEW-1 — `trading_engine_main.cpp` is a Demo, Not a Daemon

**File:** `core/engine/trading_engine_main.cpp`

The binary reads the alpha signal once, routes one batch of child orders, prints results, and returns 0. SystemD's `Restart=on-failure` would then restart it every 5 seconds, creating a loop of single-shot order attempts.

What is missing:
- No event loop (WebSocket feed subscription + book update dispatch)
- Feed handlers (`BinanceFeedHandler`, `KrakenFeedHandler`, etc.) are instantiated but immediately discarded with `(void)binance_book;`
- `NeuralAlphaMarketMaker` is never constructed or called
- `ReconciliationService` is never wired to connectors
- Heartbeat is never updated (kill switch will fire in 5 s)
- No graceful shutdown handler (SIGTERM/SIGINT)

**Industry baseline:** A production trading engine binary runs continuously, handles WebSocket reconnects, drives the market-making loop on every book update, and shuts down cleanly on signal.

**Risk:** Deploying the current binary in "live" mode would submit sporadic uncoordinated orders with no ongoing monitoring, no position management, and trigger the kill switch within seconds.

---

### C-NEW-2 — Shadow Session Configured for Wrong Exchange (`SOLANA`)

**File:** `deploy/systemd/neural-alpha-shadow.service`

```
ExecStart=... --exchanges SOLANA
```

The system targets Binance, Kraken, OKX, and Coinbase. `SOLANA` is not a supported exchange in this codebase. The shadow session as deployed:
- Cannot learn anything about intended production venue behavior
- Provides zero shadow data for go/no-go evaluation on BTCUSDT
- The shadow validation checklist in `DEPLOYMENT.md` (IC > 0.02, fill rate > 30%) is based on data from a shadow session that never ran on the right exchanges

**Industry baseline:** Shadow validation must run on identical instruments and venues as intended live trading.

---

### C-NEW-3 — Deployment Bootstrap Only Loads Binance Credentials

**File:** `deploy/aws/userdata.sh` (lines 54–70)

The bootstrap script fetches `trading/binance_api_keys` and writes only `BINANCE_API_KEY` and `BINANCE_API_SECRET` to `/etc/trading/env`. The `trading-engine.service` starts with `--venues BINANCE,KRAKEN,OKX,COINBASE`, but the environment file has no `KRAKEN_API_KEY`, `OKX_API_KEY`, or `COINBASE_API_KEY`. `LiveConnectorBase::connect()` would return `AUTH_FAILED` for 3 of 4 connectors immediately.

`DEPLOYMENT.md` Step 4 correctly documents all 8 keys, but `userdata.sh` does not implement this — a classic docs/code divergence that only surfaces at deploy time.

---

## High Issues (Must Be Fixed Before Live, Acceptable During Short Shadow)

### H4 — No Production SLOs, No Hard Alerts (Still Open from Prior Audit)

**TODOS.md H4** remains unchecked. There are no defined:
- Feed integrity SLOs (max acceptable staleness, reconnect SLA)
- Reject spike alerting thresholds
- Reconciliation drift alert triggers
- Risk circuit breaker frequency dashboards
- Prometheus scrape configs or CloudWatch metric filters for any of the above

Without these, shadow mode produces data you cannot act on systematically. You will not know when something is silently degraded.

---

## Medium Issues

### M2 — Feed Certification Replay Harness Missing (Still Open)

No pathological scenario replay for any of the 4 feed handlers. Current feed tests are unit tests against process_message() but do not cover:
- Prolonged gap with buffered backlog that forces resnapshot mid-sequence
- Duplicate sequence IDs from venue
- WS reconnect races under load

### M3 — Model Governance Not Implemented (Still Open)

No champion/challenger registry, no automatic rollback policy, no drift-triggered safe mode. Once live training is running daily, a model regression has no automatic safety net.

---

## Confirmed Closed Issues (Not a Gap Anymore)

| Item | Evidence |
|---|---|
| OKX CRC32 checksum | `okx_feed_handler.cpp:12-22` — `crc32_bytes()` standard implementation; `validate_checksum()` declared in header |
| Coinbase protocol depth | `coinbase_feed_handler.hpp` — L2 `level2` feed is consistent with the order book design (L3 not needed for this strategy) |
| Risk not wired pre-trade | `trading_engine_main.cpp:117-134` — `check_drawdown` and `commit_order` are explicit gates before `submit_order` |
| Reconciliation: no canonical diff | `reconciliation_service.hpp:196-213` — canonical fetcher path implemented; `evaluate_drift()` performs full comparison |
| Fill reconciliation | `reconciliation_service.hpp` — `FillLedger` + dedup key logic present |
| Idempotency journal | `idempotency_journal.hpp` — 256-entry pool, file-backed recovery, state machine transitions |
| Staged remediation | `reconciliation_service.hpp` — retry budgets per mismatch class, cancel-all + risk-halt hooks |

---

## Industry Standard Comparison (Updated)

| Dimension | This Project | Boutique Crypto Shop | Tier-1 HFT |
|---|---|---|---|
| Order book design | ✅ Production quality | ✅ | ✅ |
| Kill switch | ✅ Excellent | ✅ | ✅ |
| Feed handlers (4 venues) | ✅ Implemented | ✅ | ✅ |
| OKX checksum | ✅ Implemented | ✅ | ✅ |
| Live connectors (4 venues) | ✅ Implemented | ✅ | ✅ |
| Auth (HMAC per exchange) | ✅ Correct | ✅ | ✅ |
| Idempotency journal | ✅ Implemented | Sometimes | ✅ |
| Reconciliation | ✅ Implemented | Sometimes | ✅ |
| **Trading engine event loop** | ❌ **Missing** | ✅ | ✅ |
| **Shadow on correct venues** | ❌ **Wrong exchange** | ✅ | ✅ |
| **Deployment credential wiring** | ❌ **Binance only** | ✅ | ✅ |
| SLO/alert definitions | ❌ Missing | Varies | ✅ |
| ML signal quality | ✅ Good | Varies | Varies |
| Walk-forward validation | ✅ Yes | Rarely | ✅ |
| Shadow trading infra | ✅ Code is solid | Rarely | ✅ |
| Hardware timestamps | ❌ No | Sometimes | ✅ |
| Kernel bypass | ❌ No | Rarely | ✅ |

---

## Shadow-Mode Go/No-Go Decision

**NO-GO for shadow-mode deployment until C-NEW-1, C-NEW-2, and C-NEW-3 are resolved.**

These are straightforward fixes, not architectural problems. The underlying component quality is high. But deploying with a demo engine binary, on the wrong exchange, with missing credentials is not shadow trading — it is a misconfigured one-shot process.

**Estimated fix time for the three blockers:** 1–2 days of focused work.

---

## Recommended Remediation Sequence

1. **(C-NEW-1)** Replace `trading_engine_main.cpp` with a real event-driven daemon:
   - Construct feed handlers + book managers for all 4 venues
   - Run WebSocket event loop (or thread-per-feed)
   - On each book update: call `NeuralAlphaMarketMaker::on_book_update()`
   - Periodic reconciliation call (every 30 s)
   - Heartbeat keepalive for kill switch
   - Clean SIGTERM/SIGINT handler

2. **(C-NEW-2)** Fix `deploy/systemd/neural-alpha-shadow.service`: change `--exchanges SOLANA` to `--exchanges BINANCE` (or BINANCE,KRAKEN per intended shadow scope). Run shadow for ≥ 5 days before enabling live.

3. **(C-NEW-3)** Fix `deploy/aws/userdata.sh`: load all 4 exchange keys from Secrets Manager and write them all to `/etc/trading/env`.

4. **(H4)** Define minimum viable SLO alerting before shadow: feed staleness alert (>500ms), daily IC check, reconciliation mismatch CloudWatch alarm.

5. **(M2, M3)** Feed replay harness and model governance — required before live, acceptable to defer through shadow period.
