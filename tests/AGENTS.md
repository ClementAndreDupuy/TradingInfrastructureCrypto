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
