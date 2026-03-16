# tests/

Three-tier test pyramid: unit → integration → replay.

## Tiers

- **unit/** — Fast, isolated, deterministic. C++ with GTest, Python with pytest. Run on every commit.
- **integration/** — Cross-module interactions (feed → book → IPC chain). Medium speed.
- **replay/** — Full-stack regression with recorded WebSocket data. Slow, run nightly. Critical for catching silent book divergence.

## Running Tests

```bash
# C++ unit tests
cd build && make test

# Python unit tests
pytest tests/unit/ -v

# Coverage
pytest --cov=research tests/unit/
```

## Coverage Requirements

- `core/` — 90%+ (critical path)
- `research/` — 70%+

## Kill Switch Drill

Run weekly: trigger software kill switch → verify all orders canceled → verify no new orders accepted → reset.

## Test Data

Store recorded feed messages in `tests/data/` for replay tests. Never commit large data files — use `.gitignore`.

## Reusable Agent Memory (Updated)

- Use tiered execution to keep feedback fast:
  1. `tests/unit/` during iteration
  2. `tests/integration/` before commit
  3. `tests/replay/` for market-data correctness
- Add/refresh `tests/perf/` cases when latency-sensitive code changes in `core/`.
- Replay fixtures should favor determinism and compact size; keep large raw captures out of git.
