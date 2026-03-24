## Alpha signal bridge: Python neural model → C++ strategy via mmap file.

Layout of /tmp/trt_ipc/neural_alpha_signal.bin (32 bytes):
   offset  0 : uint64  seq         — seqlock counter (even = stable, odd = writer active)
   offset  8 : float64 signal_bps  — mid-horizon return prediction (bps)
   offset 16 : float64 risk_score  — adverse-selection probability [0, 1]
   offset 24 : int64   ts_ns       — nanosecond timestamp of last update

Python writes via the seqlock protocol (shadow_session._SignalPublisher):
   1. increment seq to odd  (signals write-in-progress to readers)
   2. write signal_bps, risk_score, ts_ns
   3. increment seq to even (signals write-complete)


C++ reads via seqlock: load seq1 (must be even) → load fields →
   load seq2 (must equal seq1); retry on mismatch or odd seq.
   Each read is < 100 ns; called on every book update.

Gate logic:

- LONG  trade allowed only when signal_bps > signal_min_bps AND risk_score < risk_max 
- SHORT trade allowed only when signal_bps < -signal_min_bps AND risk_score < risk_max 
- NEUTRAL (|signal| < min_bps) → allow market-maker only (no directional taker arb)
- STALE  (age > stale_ns)      → allow taker arb (fail-open to preserve uptime)


## Classes & Methods (Quick Reference)

- **`AlphaSignalReader` (`alpha_signal.hpp`)** — Reads neural alpha signal frame via seqlock mmap.
  - `open()/close()/is_open()`: Manages shared-memory file mapping lifecycle.
  - `read()`: Returns a consistent alpha frame (`signal_bps`, `risk_score`, sizing, horizon, timestamp).
  - `allows_long()/allows_short()/allows_mm()`: Applies signal/risk/staleness gating with fail-open behavior.

- **`RegimeSignalReader` (`regime_signal.hpp`)** — Reads regime probabilities from mmap.
  - `open()/close()/read()`: Accesses and validates seqlock regime frame data.
  - `is_stale(...)`: Checks frame age against caller-provided freshness threshold.

- **`LobPublisher` (`lob_publisher.hpp`)** — Publishes normalized top-of-book snapshots to shared memory.
  - `open()/close()/is_open()`: Initializes and manages the ring-buffer mapping.
  - `publish(...)`: Writes a single exchange/symbol top-5 LOB snapshot into the next ring slot.
