# Portfolio Management Audit: Short/Long from Alpha Signals

**Date:** 2026-03-31  
**Branch:** `claude/audit-portfolio-management-5ZiQk`  
**Scope:** Full signal-to-position pipeline — from `AlphaSignalReader` through `PortfolioIntentEngine`, `PositionLedger`, `SmartOrderRouter`, and order submission in `trading_engine_main.cpp`.

---

## Executive Summary

The overall architecture is sound: a seqlock-framed shared-memory IPC feeds alpha and regime signals into a well-structured `PortfolioIntentEngine` that produces typed `PortfolioIntent` objects consumed by the main loop. However, the audit found **two critical runtime bugs** that would cause severe misbehavior in live production today, plus several high-severity issues around short-side correctness, staleness handling, and futures risk enforcement.

---

## Findings

### CRITICAL-1 — Live config uses wrong YAML keys for regime thresholds → all thresholds are zero at runtime

**Files:**
- `config/live/portfolio.yaml`
- `core/engine/algo_config_loader.hpp:123–127`
- `core/execution/common/portfolio/portfolio_intent_engine.hpp:239–258`

**Detail:**

The live config YAML uses `shock_flatten_threshold` and `illiquid_reduce_threshold` (legacy names), but `AlgoConfigLoader::load_portfolio` looks for `shock_enter_threshold`, `shock_exit_threshold`, `illiquid_enter_threshold`, `illiquid_exit_threshold`, and `regime_persistence_ticks`. The `apply_double` / `apply_int` helpers silently no-op on missing keys, so all five fields remain at whatever value `PortfolioIntentConfig portfolio_cfg;` (declared without `= {}`) holds — undefined behaviour from uninitialized stack memory.

In the typical case on Linux/x86-64 stack memory is zero, which makes the consequences deterministic and severe:

| Field | Live YAML | Loaded value | Effect |
|---|---|---|---|
| `shock_enter_threshold` | *(absent)* | 0.0 | `p_shock >= 0.0` always true |
| `illiquid_enter_threshold` | *(absent)* | 0.0 | `p_illiquid >= 0.0` always true |
| `regime_persistence_ticks` | *(absent)* | 0 | `ticks (1) >= 0` → activates on first evaluation |

Tracing `_update_regime_hysteresis` with `persistence = 0`:
```
tick N   (inactive): ticks = 1   → 1 >= 0 → active = true,  ticks = 0
tick N+1 (active):   ticks = 0   → 0 >= 0 → active = false, ticks = 0
tick N+2 (inactive): ticks = 1   → 1 >= 0 → active = true,  …
```
Result: `shock_regime_active_` oscillates true/false every tick. On every odd tick, the engine hits the `if (risk_off || negative_reversal || shock_regime || …)` flatten branch:
```cpp
out.flatten_now   = std::abs(current_position) > 0.0;
out.urgency       = ShadowUrgency::AGGRESSIVE;
out.position_delta = -current_position;
```
This would generate constant aggressive flatten orders every ~500 ms while any position is open.

**Fix:** Add the missing keys to `config/live/portfolio.yaml`, and zero-initialize the config struct:
```cpp
PortfolioIntentConfig portfolio_cfg{};   // was: PortfolioIntentConfig portfolio_cfg;
```
Also add a startup assertion that both thresholds are non-zero before entering the main loop.

---

### CRITICAL-2 — Live mode never populates `oldest_inventory_age_ms` → stale inventory exit never fires

**Files:**
- `core/engine/trading_engine_main.cpp:912–918` (`build_live_portfolio_snapshot`)
- `core/execution/common/portfolio/portfolio_intent_engine.hpp:120–121`

**Detail:**

In live mode the engine snapshot is built by `build_live_portfolio_snapshot`, which sums reconciliation positions but never sets `oldest_inventory_age_ms`. The field stays zero. As a result:
```cpp
const bool stale_inventory = ledger.oldest_inventory_age_ms >= cfg_.stale_inventory_ms &&
                             std::abs(current_position) > 0.0;
// → 0 >= 5000 → always false
```
The `stale_inventory_ms: 5000` protection configured in `config/live/portfolio.yaml` is completely inoperative in live mode. A position can age indefinitely without triggering a forced exit or a size reduction. Shadow mode is unaffected because it reads `shadow_engine.inventory_age_ms()`.

**Fix:** The `PositionLedger` class properly tracks per-venue `opened_at` timestamps. The live path should use `PositionLedger::snapshot()` (which does populate `oldest_inventory_age_ms`) rather than the reconciliation-only `build_live_portfolio_snapshot`. Alternatively, `build_live_portfolio_snapshot` should be extended to carry inventory age from the `ReconciliationSnapshot`, if that data is available.

---

### HIGH-1 — `edge_positive` gate applied to longs but not shorts → ALPHA_NEGATIVE reason emitted when alpha_scale = 0

**File:** `core/execution/common/portfolio/portfolio_intent_engine.hpp:126,154–175`

**Detail:**

Long entry requires both the signal threshold **and** a positive edge over expected cost:
```cpp
const bool edge_positive    = alpha_signal.signal_bps > expected_cost_bps;   // only meaningful for longs
const bool positive_entry   = alpha_signal.signal_bps >=  cfg_.min_entry_signal_bps && edge_positive;
const bool negative_entry   = alpha_signal.signal_bps <= -cfg_.min_entry_signal_bps && !cfg_.long_only;
```
`edge_positive` is always false for a negative signal (e.g., `-3.0 > 4.0` is false), so it is never checked for `negative_entry`. The `alpha_scale` formula does independently zero out magnitude when `|signal| < cost`, but reason-code logging happens before this:

```cpp
// signal_bps = -2.5, expected_cost = 4.0
// negative_entry = true   (meets threshold)
// alpha_scale = clamp((2.5 - 4.0) / 2.0, 0, 1) = 0.0
// magnitude = 0  →  target = 0  (no position)
// BUT: ALPHA_NEGATIVE reason code already appended
```
A consumer observing `ALPHA_NEGATIVE` in logs/metrics will believe a short entry was warranted, when in fact zero position was targeted. Under long-running shadow backtests this pollutes signal-edge statistics.

For longs under the same conditions the reason would be `ALPHA_DECAY`, which is the correct label.

**Fix:** Apply the symmetric edge check for shorts:
```cpp
const bool negative_entry = alpha_signal.signal_bps <= -cfg_.min_entry_signal_bps
                             && !cfg_.long_only
                             && std::abs(alpha_signal.signal_bps) > expected_cost_bps;
```

---

### HIGH-2 — `AlphaSignalReader` member `fd_` and `ptr_` not initialized in constructor

**File:** `core/ipc/alpha_signal.hpp:28–31, 134–138`

**Detail:**

The constructor sets `path_`, `signal_min_bps_`, and `risk_max_` but leaves `fd_` and `ptr_` without initialization:
```cpp
explicit AlphaSignalReader(const std::string &path, double signal_min_bps, double risk_max)
    : path_(path), signal_min_bps_(signal_min_bps), risk_max_(risk_max) {
}   // fd_ and ptr_ are indeterminate
```
The destructor calls `close()`, which checks `if (fd_ >= 0)` — undefined behaviour if `open()` was never called, since `fd_` holds stack garbage. On common architectures this could issue `::close()` on a valid file descriptor belonging to another part of the process.

**Fix:**
```cpp
int fd_        = -1;
const char *ptr_ = nullptr;
```

---

### HIGH-3 — `allows_long()` / `allows_short()` fail-open when IPC reader not connected

**File:** `core/ipc/alpha_signal.hpp:97–113`

**Detail:**

```cpp
bool allows_long() const noexcept {
    if (!ptr_)
        return true;   // ← permits all long trades when shm file absent
    …
}
bool allows_short() const noexcept {
    if (!ptr_)
        return true;   // ← permits all short trades when shm file absent
    …
}
```
If the alpha shared-memory file (`/tmp/trt_ipc/trt_alpha.bin`) is unavailable at startup or disappears at runtime, both gates allow all trades unconditionally. `SmartOrderRouter::route_with_alpha` uses these gates as a pre-trade filter; failing open removes the signal-quality gate entirely.

This is a deliberate design choice but it creates an unmonitored failure mode. There is no log warning emitted when `!ptr_`, so the operator has no indication that trades are ungated.

**Recommendation:** At minimum, emit a throttled `LOG_WARN` when `!ptr_` inside these methods, and consider whether fail-open is the correct policy for the alpha path vs. the regime path (which auto-reconnects inside `read()`).

---

### HIGH-4 — Futures risk gate (`commit_futures_order`) not wired in order submission path

**File:** `core/engine/trading_engine_main.cpp:787–792`

*(Previously documented in `docs/FUTURES_SPOT_REVIEW_2026-03-29.md`. Included here for completeness.)*

**Detail:**

In both spot and futures modes, order submission calls `global_risk.commit_order(...)`. The futures-specific `GlobalRiskControls::commit_futures_order(...)` — which enforces leverage caps, maintenance margin, and mark/index deviation — is never invoked. Futures-specific checks are fully implemented but unreachable.

**Fix:** In the `futures_only_mode` branch, replace `commit_order` with `commit_futures_order`, populating `FuturesRiskContext` with the current leverage, mark price, and funding rate from `BinanceFuturesConnector`.

---

### HIGH-5 — `negative_reversal` threshold creates train/prod asymmetry for shadow shorts

**Files:**
- `config/live/portfolio.yaml:4` (`negative_reversal_signal_bps: -1.0`)
- `config/shadow/portfolio.yaml:4` (`negative_reversal_signal_bps: -3.0`)
- `core/execution/common/portfolio/portfolio_intent_engine.hpp:117,145`

**Detail:**

In **shadow** (training) mode, short entries are enabled for `signal_bps <= -2.0` and the flatten-on-reversal trigger fires at `signal_bps <= -3.0`. A short position can therefore exist while `signal_bps` is between `–2.0` and `–3.0`.

In **live** mode, `long_only: true` prevents all short entries, but the reversal threshold is `–1.0`. Any live long position is flattened when `signal_bps <= –1.0`. Because the model is trained in a regime where a –2.0 bps signal is a valid short entry, live mode discards profitable short-entry signals as flatten triggers.

There is no symmetric `positive_reversal_signal_bps` parameter. A short position in shadow mode is not explicitly exited by a bullish signal reversal via a reason code — it relies on the `positive_entry` path flipping the target sign (urgency will be AGGRESSIVE due to `delta >= 0.30`), which is functionally correct but lacks observability.

**Recommendation:**
1. Add `positive_reversal_signal_bps` to the config and engine, symmetric with `negative_reversal_signal_bps`.
2. Document the live vs. shadow threshold discrepancy as an intentional conservative live policy, or align the thresholds.

---

### MEDIUM-1 — `ALPHA_NEGATIVE` reason code appended before deadband zero-out

**File:** `core/execution/common/portfolio/portfolio_intent_engine.hpp:169–178`

**Detail:**

```cpp
target = positive_entry ? magnitude : -magnitude;
append_reason(out, positive_entry ? ALPHA_POSITIVE : ALPHA_NEGATIVE);   // ← reason appended here

if (std::abs(alpha_signal.signal_bps) <= cfg_.deadband_signal_bps)
    target = 0.0;   // target zeroed, but reason code already committed
```
When `signal_bps` is within the deadband (e.g., `0.8 bps`) but still exceeds `–min_entry_signal_bps` for short entry (only relevant if `long_only = false`), the reason code `ALPHA_NEGATIVE` is appended but the actual target is clamped to zero. Downstream log consumers and metrics dashboards will see `ALPHA_NEGATIVE` intent with a zero delta.

**Fix:** Move the deadband check before the reason-code append, or append `ALPHA_DECAY` when `|signal| <= deadband`.

---

### MEDIUM-2 — Market maker posts ask (short-initiating) orders without checking `long_only`

**File:** `core/execution/market_maker.hpp:170–176`

**Detail:**

```cpp
bool post_bid = (net_pos + qty) <= cfg_.max_position;
bool post_ask = (net_pos - qty) >= -cfg_.max_position;

if (post_bid) bid_id_ = submit_limit(Side::BID, bid_px, qty);
if (post_ask) ask_id_ = submit_limit(Side::ASK, ask_px, qty);
```
`NeuralAlphaMarketMaker` does not read `long_only` from `PortfolioIntentConfig`. When the market maker is running concurrently with `long_only = true`, it will post ask quotes that, if filled, would push the net position below zero — directly violating the live constraint. The `PortfolioIntentEngine` would then try to flatten, creating a race.

**Fix:** Add a `long_only` flag to `MarketMakerConfig` and gate `post_ask` on `!long_only || net_pos > 0`.

---

### MEDIUM-3 — Three independent staleness thresholds with no consistent configuration source

**Files:**
- `core/ipc/alpha_signal.hpp:23` (`STALE_NS = 2'000'000'000LL` — 2 s, compile-time constant)
- `config/live/portfolio.yaml:8` (`stale_signal_ms: 1500` — 1.5 s, portfolio engine gate)
- `core/execution/market_maker.hpp:42` (`cfg_.stale_ns` — from `MarketMakerConfig`, a third value)

**Detail:**

Three components independently decide whether an alpha signal is stale:
1. `AlphaSignalReader` uses 2 s hardcoded for `allows_long()`/`allows_short()` (routing gate).
2. `PortfolioIntentEngine` uses `stale_signal_ms: 1500` from portfolio YAML (intent gate).
3. `NeuralAlphaMarketMaker` uses its own `cfg_.stale_ns` from `MarketMakerConfig` (quote gate).

A signal between 1.5 s and 2.0 s old will be considered stale by the intent engine but **not** stale by the routing gate, allowing a child order to be routed on a signal the intent engine would have rejected.

**Fix:** Expose a single `stale_signal_ms` knob in the portfolio config and thread it into `AlphaSignalReader` construction (replacing the `STALE_NS` compile-time constant) and `MarketMakerConfig`.

---

### MEDIUM-4 — Reconciliation resets inventory age for pre-existing positions

**File:** `core/execution/common/portfolio/position_ledger.hpp:131–136`

**Detail:**

```cpp
} else if (!venue->has_inventory_age) {
    venue->opened_at = std::chrono::steady_clock::now();
    venue->has_inventory_age = true;
}
```
During `reconcile_positions`, any venue with a non-zero position that lacks `has_inventory_age` (e.g. because this is the first reconciliation after startup) has its `opened_at` set to `now()`. This silently resets the age of any position that was already held before the engine started, guaranteeing it gets a fresh timer. If the engine restarts mid-position, stale-inventory protection will not fire for `stale_inventory_ms` regardless of how old the position actually is.

**Recommendation:** Include position age in `ReconciliationSnapshot` and propagate it through `reconcile_positions`, or log a warning when a non-zero position is encountered without age history.

---

### MEDIUM-5 — `futures_mid_price` populated from spot book in futures mode → basis guard inoperative

**File:** `core/engine/trading_engine_main.cpp:930`  
*(Also documented in `docs/FUTURES_SPOT_REVIEW_2026-03-29.md`.)*

```cpp
intent_ctx.futures_mid_price = futures_only_mode ? binance_book.mid_price() : 0.0;
```
Both `spot_mid_price` and `futures_mid_price` are set from `binance_book` (spot). `compute_basis_bps` returns `|futures - spot| / spot`, which will be 0.0. The `max_basis_divergence_bps: 25.0` guard is permanently inactive in futures mode.

**Fix:** Feed `BinanceFuturesConnector`'s mark price into `intent_ctx.futures_mid_price`.

---

### LOW-1 — `check_stop` ignores its `AlphaSignal` parameter

**File:** `core/execution/market_maker.hpp:193`

```cpp
void check_stop(double mid, const AlphaSignal& ) {
```
The parameter is unnamed, indicating intent to use it was either abandoned or deferred. Signal-informed stop adjustment (e.g., widening the stop when risk is elevated) is not implemented.

---

### LOW-2 — `RegimeSignalReader::read()` auto-reconnects; `AlphaSignalReader::read()` does not

**Files:**
- `core/ipc/regime_signal.hpp:62–65`
- `core/ipc/alpha_signal.hpp:67–69`

```cpp
// RegimeSignalReader
RegimeSignal read() {
    if (!ptr_ && !open()) { return {}; }   // ← auto-reconnect
    …
}
// AlphaSignalReader
AlphaSignal read() const noexcept {
    if (!ptr_) return {};   // ← no reconnect attempt
    …
}
```
The regime reader will silently recover from a missing file; the alpha reader will not. If `/tmp/trt_ipc/trt_alpha.bin` appears after engine startup, alpha signals will never be picked up (requiring a restart). The asymmetry is surprising and undocumented.

---

### LOW-3 — `intent_action` returns `"reduce"` for both reducing longs and reducing shorts

**File:** `core/engine/trading_engine_main.cpp:224–232`

```cpp
auto intent_action(const trading::PortfolioIntent &intent) -> const char * {
    if (intent.flatten_now)      return "flatten";
    if (intent.position_delta > 1e-9)  return "enter_long";
    if (intent.position_delta < -1e-9) return "reduce";
    return "hold";
}
```
A negative `position_delta` maps to `"reduce"` whether the intent is closing a long, adding to a short, or reducing a short. Logs will label a short-entry as `"reduce"`, confusing intent tracking in shadow mode.

---

## Summary Table

| ID | Severity | Component | Impact |
|---|---|---|---|
| CRITICAL-1 | **Critical** | Live config / regime hysteresis | Constant aggressive flatten on every odd tick when any position open |
| CRITICAL-2 | **Critical** | Live position snapshot | Stale inventory exit never fires in live mode |
| HIGH-1 | High | `PortfolioIntentEngine` | Misleading `ALPHA_NEGATIVE` reason when short has no edge; asymmetric entry guard |
| HIGH-2 | High | `AlphaSignalReader` | UB / potential erroneous `close()` on uninit `fd_` |
| HIGH-3 | High | `AlphaSignalReader` | Ungated order flow when IPC file unavailable, no observable warning |
| HIGH-4 | High | Main loop / futures path | Futures risk controls (leverage, margin) not enforced |
| HIGH-5 | High | Config / `PortfolioIntentEngine` | No symmetric positive-reversal exit for shorts; live/shadow threshold mismatch |
| MEDIUM-1 | Medium | `PortfolioIntentEngine` | Misleading reason code in deadband zone |
| MEDIUM-2 | Medium | `NeuralAlphaMarketMaker` | MM can build short position in long_only live mode |
| MEDIUM-3 | Medium | IPC / config | Three independent staleness thresholds, inconsistently configured |
| MEDIUM-4 | Medium | `PositionLedger` | Inventory age resets on reconciliation; post-restart stale exits delayed |
| MEDIUM-5 | Medium | Main loop / futures path | Basis guard permanently inactive in futures mode |
| LOW-1 | Low | `NeuralAlphaMarketMaker` | Unused alpha parameter in stop logic |
| LOW-2 | Low | IPC readers | Alpha reader does not auto-reconnect; regime reader does |
| LOW-3 | Low | Main loop logging | `"reduce"` label conflates reducing long, adding short, reducing short |

---

## Recommended Fix Order

1. **CRITICAL-1** — Add missing regime keys to `config/live/portfolio.yaml`; zero-initialize `portfolio_cfg{}` in main.
2. **CRITICAL-2** — Use `PositionLedger::snapshot()` for live inventory age, or populate `oldest_inventory_age_ms` in `build_live_portfolio_snapshot`.
3. **HIGH-2** — Initialize `fd_ = -1` and `ptr_ = nullptr` in `AlphaSignalReader` constructor.
4. **HIGH-1** — Apply symmetric `|signal| > expected_cost_bps` guard for `negative_entry`.
5. **HIGH-4** — Wire `commit_futures_order` in futures submission path.
6. **MEDIUM-2** — Add `long_only` flag to `MarketMakerConfig` and gate ask-side quoting.
7. **MEDIUM-3** — Unify staleness threshold source across all three consumers.
8. **HIGH-3** — Add logged warning in `allows_long()`/`allows_short()` when `!ptr_`.
