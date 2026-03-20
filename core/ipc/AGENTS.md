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