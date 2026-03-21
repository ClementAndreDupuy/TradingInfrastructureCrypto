## TODO List

---

### CRITICAL

#### [ ] 1. Shadow session timestamp integrity and metrics correctness
**Problem**
- Recent BTCUSDT shadow output reported a session duration of 2472.0 minutes for a configured 2000 second run, which means the current reporting pipeline is mixing incompatible timestamps or accepting non-monotonic event times.
- This makes headline shadow metrics such as duration, realised IC, ICIR, and drift windows unreliable, because signal/outcome alignment depends on correct temporal ordering.
- The runtime currently logs `timestamp_ns` from the last tick used for inference, while the summary computes elapsed time from the first and last logged timestamps. If those timestamps come from heterogeneous exchange/local sources, the report becomes invalid.

**Why this matters**
- We cannot trust any shadow readiness conclusion while the time axis is corrupted.
- DriftGuard, canary checks, and post-run analytics can all fire on incorrect timing assumptions.
- A bad metrics pipeline risks promoting or rejecting models for the wrong reasons.

**Required work**
- Separate and log `timestamp_exchange_ns`, `timestamp_local_ns`, and a strictly monotonic/session-local sequence counter for every shadow signal event.
- Ensure runtime freshness and duration calculations use the correct clock semantics (`system_clock` for wall time, `steady_clock` for elapsed time/freshness, never mixing the two).
- Make the summary/report code compute duration from session-local timing rather than exchange timestamps.
- Add validation that rejects or flags non-monotonic timestamps and cross-venue timestamp jumps.
- Extend the report with explicit timestamp-quality diagnostics.

**Acceptance criteria**
- A 2000 second shadow run reports a duration within a small tolerance of the configured run window.
- Shadow JSONL records contain distinct exchange and local timestamps plus a monotonic event index.
- Summary generation flags malformed/non-monotonic timestamps instead of silently using them.
- IC/ICIR computation uses deterministic signal/outcome ordering documented in code and tests.
- Unit/integration coverage exists for mixed timestamp inputs and out-of-order event scenarios.

#### [ ] 2. Binance feed synchronization hardening (sequence gaps + buffer overflow)
**Problem**
- The Binance BTCUSDT shadow capture shows repeated sequence gaps immediately after snapshot application and frequent `Buffer overflow` re-snapshot events.
- This indicates the snapshot/delta handoff is not robust enough under live BTCUSDT update pressure.
- Chronic re-snapshot loops degrade both the C++ strategy view and the Python shadow model input quality.

**Why this matters**
- Order-book integrity is foundational; if Binance is unstable, downstream shadow metrics are polluted.
- Excessive re-snapshoting increases latency, drops continuity, and amplifies venue imbalance in the model input stream.
- This is likely one of the largest contributors to poor shadow final results.

**Required work**
- Audit the full Binance snapshot/delta sync path against the official exchange sequencing contract.
- Instrument buffer high-water mark, snapshot latency, applied/skipped buffered deltas, and re-sync root causes.
- Replace raw string buffering with a more explicit parsed delta queue if it improves correctness/latency.
- Revisit buffer size, backpressure handling, and re-sync behavior under peak BTCUSDT update bursts.
- Add replay/integration tests that reproduce immediate post-snapshot gaps and sustained burst traffic.

**Acceptance criteria**
- Shadow/replay tests demonstrate stable snapshot-to-stream transition without immediate sequence-gap failure.
- Buffer overflow rate for BTCUSDT is reduced to near-zero in normal shadow operation and becomes a measurable alert when it happens.
- Logs expose buffer high-water mark, re-snapshot counts, and explicit gap reasons.
- Replay tests cover both nominal bursts and pathological sequencing edge cases.
- The feed can run a multi-hour shadow session without recurring resync loops under typical market conditions.

#### [ ] 3. Shadow observability upgrade for training, gating, and venue quality
**Problem**
- The current shadow workflow relies heavily on stdout prints, and Python is launched in the background, which makes trainer output easy to miss.
- Final reports expose only coarse summary stats and hide the most actionable failure modes: venue coverage, retrain cadence, gating reasons, drift triggers, and safe-mode root causes.
- This slows diagnosis and leads to speculation instead of evidence.

**Why this matters**
- We need operator-grade telemetry before tuning the alpha or making readiness decisions.
- Without structured logs, it is hard to distinguish model weakness from feed weakness, drift protection, or configuration mistakes.

**Required work**
- Add structured JSONL events for bootstrap training start/end, fold count, epoch metrics, selected checkpoint, continuous retrain triggers, drift breaches, canary rollbacks, and safe-mode activation.
- Add per-venue counters: ticks received, ticks used, missing-venue incidents, REST fallback usage, resnapshot counts, and feed startup failures.
- Include gating-reason breakdowns (confidence gate, horizon-disagreement gate, safe-mode gate) in runtime and summary outputs.
- Produce a single post-run shadow health section that surfaces data quality and model quality separately.

**Acceptance criteria**
- A shadow run emits machine-readable training and operations events without depending on unbuffered stdout.
- The final report includes per-venue quality diagnostics and gating/safe-mode breakdowns.
- An operator can identify why signal coverage is low without reading raw console logs.
- Missing venue participation, retrain failures, and drift/canary events are all visible in structured logs.
- Documentation explains how to inspect the new shadow telemetry.

### HIGH

#### [ ] 4. Feed bootstrap ordering and bridge wiring correctness
**Problem**
- `trading_engine_main.cpp` previously started feed handlers before registering `BookManager` snapshot/delta callbacks.
- That startup ordering can drop the initial synchronized snapshot and early deltas, producing the exact failure mode where WebSocket connections are healthy but the bridge sees no book updates.
- The same bootstrap path also initialized `BookManager` grids from fallback tick sizes before any venue tick-size refresh had run.

**Why this matters**
- The bridge and downstream strategy stack depend on receiving the first valid snapshot immediately.
- Losing the bootstrap snapshot can make venue health logic classify a working feed as dead.
- Incorrect initial grid sizing weakens venue-native book fidelity.

**Required work**
- Keep callback registration strictly ahead of feed startup in every engine/bootstrap path.
- Add regression coverage for feed start -> snapshot callback -> bridge publish ordering.
- Ensure venue tick sizes are refreshed before constructing `BookManager` grids.
- Audit any alternate startup surfaces to ensure they follow the same ordering.

**Acceptance criteria**
- A started feed publishes its first synchronized snapshot into `BookManager`/LOB bridge without requiring a later delta.
- Engine startup tests fail if callbacks are registered after `start()`.
- Book grids use exchange-derived tick sizes when public metadata fetch succeeds.

#### [ ] 4. Stronger bootstrap training and realistic continuous retraining cadence
**Problem**
- Current shadow defaults bootstrap from only 1000 ticks and 3 epochs, while continuous retraining is configured to wait 4000 ticks.
- In shorter shadow sessions, continuous retraining may never fire, so results are dominated by a very small cold-start training sample.
- This is especially weak for multi-horizon labels and walk-forward validation.

**Why this matters**
- Under-training causes unstable signals, over-gating, poor calibration, and fragile drift behavior.
- A cold-start model trained on too little data is not a credible shadow benchmark.

**Required work**
- Revisit default bootstrap sizes for BTCUSDT and other liquid pairs.
- Lower continuous retrain thresholds so retraining actually occurs during realistic shadow windows.
- Distinguish between minimum debugging settings and production-shadow defaults.
- Log effective training sample size, folds used, and whether continuous retrain was triggered or skipped.

**Acceptance criteria**
- Default production-shadow config uses materially larger bootstrap data than the current 1000 ticks / 3 epochs.
- Continuous retraining triggers during a standard shadow session unless explicitly disabled.
- Reports clearly state how many training ticks, epochs, folds, and retrains were actually used.
- Documentation differentiates quick-debug mode from real shadow validation mode.

#### [ ] 5. Coinbase credential validation and degraded-mode handling
**Problem**
- Coinbase Advanced Trade level2 WebSocket auth requires a valid EC private-key PEM for JWT signing.
- Users are likely to paste the wrong credential format, causing the C++ feed to hard-fail even though Python can still use Coinbase public REST for training data.
- Today this failure can make a run look like a four-venue shadow when it is effectively operating with degraded live venue coverage.

**Why this matters**
- Coinbase failures should be explicit and actionable, not ambiguous.
- We need a clean separation between “Coinbase live feed validated” and “Coinbase unavailable, REST-only fallback for research data.”

**Required work**
- Add a preflight credential validator for Coinbase that checks PEM parseability and key type before the shadow session starts.
- Improve operator-facing error messages to explain exactly which credential format is required and where it is used.
- Introduce an explicit degraded-mode classification in reports when Coinbase live level2 is unavailable.
- Ensure training/reporting clearly distinguishes public REST fallback from authenticated WebSocket participation.

**Acceptance criteria**
- Shadow startup fails fast with a precise Coinbase credential-format error before attempting the full run, or explicitly degrades with visible warnings if configured to continue.
- Reports clearly state whether Coinbase participated as a live C++ level2 feed, a research-only REST source, or not at all.
- Operators can validate Coinbase credentials before starting a multi-hour shadow session.
- Run metadata preserves the exact venue participation mode for post-run analysis.

#### [ ] 6. Venue quorum and synchronized warm-start before enabling inference
**Problem**
- Early shadow inference can begin while venue participation is uneven, especially when one feed is delayed or failing.
- This creates unstable early-session model state and makes cross-venue comparisons noisy.
- The concern that “all exchanges should have the same ticks at the beginning” reflects a real warm-start consistency problem.

**Why this matters**
- The first inference windows strongly influence drift stats, safe-mode triggers, and operator confidence.
- Uneven venue warmup makes the model learn and infer on an inconsistent market state representation.

**Required work**
- Define venue quorum rules for production-shadow startup.
- Require each enabled venue to contribute a minimum number of valid synchronized ticks before publishing non-neutral signals.
- Add explicit warmup states and logs that explain why inference is still neutral.
- Allow intentional degraded-mode runs, but mark them as such in both runtime logs and final reports.

**Acceptance criteria**
- Non-neutral inference begins only after venue quorum and warmup thresholds are satisfied, unless degraded mode is explicitly enabled.
- Shadow logs/report show warmup progress per venue.
- Early-session neutral signals are attributable to warmup state rather than appearing as unexplained gating.
- Degraded-mode runs are clearly labeled and not confused with full venue validation.

### MEDIUM

#### [ ] 7. Calibrate gating and safe-mode thresholds from empirical shadow distributions
**Problem**
- Recent shadow output showed 98.4% gated ticks and 65.5% safe-mode incidence, which suggests current thresholds are too strict for live data quality and model calibration.
- Hard-coded confidence and horizon-agreement gates may be rejecting nearly all signals instead of filtering only weak ones.

**Why this matters**
- A model that is almost always neutralized cannot be meaningfully evaluated for alpha quality.
- Over-gating hides whether the core model is bad or just miscalibrated.

**Required work**
- Analyse raw signal distributions, direction confidence, and gate hit rates on shadow logs.
- Tune confidence thresholds, horizon-agreement rules, and safe-mode settings using offline calibration rather than intuition.
- Track gating reasons separately in reports and dashboards.
- Add experiments comparing raw-signal IC to post-gate IC.

**Acceptance criteria**
- Reports show gate hit rates by reason and compare pre-gate vs post-gate signal quality.
- Threshold changes are justified by measured calibration results.
- Production-shadow defaults no longer result in near-total signal suppression under normal market conditions.
- Threshold tuning is documented and reproducible.

#### [ ] 8. Fixed research-space book normalization across venues
**Problem**
- Venue-native order books use different tick sizes and therefore different raw depth granularity.
- Although the engine correctly preserves venue-native grids, the research layer still needs stronger normalization so models compare equivalent economic depth across venues.

**Why this matters**
- Comparable feature representations improve cross-venue generalization and reduce early-session inconsistency.
- This addresses the real concern behind mismatched `max_levels` without corrupting venue-correct order-book construction.

**Required work**
- Evaluate resampling LOB features into fixed bps-from-mid buckets or another venue-agnostic representation.
- Ensure each venue contributes the same effective depth horizon to the model.
- Measure whether normalized representations improve IC, gating rate, and robustness.

**Acceptance criteria**
- Research experiments compare the current feature representation against a venue-normalized alternative.
- Results document whether the new representation improves cross-venue stability and alpha quality.
- Engine-side native tick correctness remains unchanged.

### LOW

#### [ ] 9. Shadow run mode taxonomy and report labeling cleanup
**Problem**
- Current shadow outputs can blur together full-venue validation, degraded runs, debug runs, and bootstrap-only runs.
- This makes historical comparisons harder and can cause operators to over-trust weak sessions.

**Why this matters**
- Good naming and report labeling prevent accidental misuse of shadow results.

**Required work**
- Define canonical run modes: debug, degraded, bootstrap-only, full production-shadow.
- Stamp each run with mode metadata and required readiness expectations.
- Surface mode labels in logs, summaries, and archived artifacts.

**Acceptance criteria**
- Every shadow report clearly states run mode and venue participation class.
- Debug/degraded runs cannot be mistaken for full validation runs.
- Historical run comparisons can filter by mode.

### RESEARCH

#### [ ] 10. Horizon/label redesign for live shadow tradeability
**Problem**
- The current label stack spans horizons up to 500 ticks, but live shadow quality may be dominated by shorter-term microstructure effects and venue instability.
- Horizon disagreement currently gates many signals, suggesting potential mismatch between training targets and live execution cadence.

**Why this matters**
- Better horizon design can improve both raw IC and post-gate tradeability.
- This is a research problem, but it likely has large downstream impact.

**Required work**
- Run offline studies on alternative horizon sets, target definitions, and calibration schemes.
- Compare alpha quality and gating efficiency for shorter, venue-aware horizons.
- Evaluate whether adverse-selection labels and regime interaction should be redesigned alongside horizon updates.

**Acceptance criteria**
- Research artifacts compare current and proposed horizon stacks on the same datasets.
- Any change to production targets is backed by walk-forward and shadow evidence.
- The chosen target design improves usable signal coverage without degrading realised IC.
