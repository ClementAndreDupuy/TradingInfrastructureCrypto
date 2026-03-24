# core/ipc/

Shared-memory bridges between research/runtime processes and C++ strategy/feed surfaces.

## Classes & Methods (Quick Reference)

- **`AlphaSignalReader` (`alpha_signal.hpp`)** — Seqlock mmap reader for neural alpha frames.
  - `open()/close()/is_open()`: Manages mmap file lifecycle.
  - `read()`: Reads latest consistent signal/risk/size/horizon/timestamp payload.
  - `allows_long()/allows_short()/allows_mm()`: Convenience gates using signal/risk and staleness rules.

- **`RegimeSignalReader` (`regime_signal.hpp`)** — Seqlock mmap reader for regime probabilities.
  - `open()/close()/read()`: Mapping lifecycle + consistent frame reads.
  - `is_stale(...)`: Staleness check for regime frame timestamps.

- **`LobPublisher` (`lob_publisher.hpp`)** — Single-writer mmap ring publisher for top-of-book snapshots.
  - `open()/close()/is_open()`: Initializes and manages shared-memory ring state.
  - `publish(...)`: Writes one exchange/symbol top-5 book frame.
