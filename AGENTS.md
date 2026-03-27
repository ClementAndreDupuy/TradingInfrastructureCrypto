# AGENTS.md

Guidance for agents in this repository (compatible with Codex AGENTS.md and Claude Code).

Note: `AGENTS.md` is the canonical source. `CLAUDE.md` files are mirrored for Claude Code compatibility.

## System Overview

Microsecond-latency crypto trading system targeting Binance, Kraken, OKX, and Coinbase.

**Architecture split:**
- **C++ hot path** — order book, execution, risk (latency-critical, no Python).
- **Python cold path** — research, backtesting, monitoring.

## Core Agent Workflow

The agent operating model is documented in [`docs/WORKFLOW.md`](docs/WORKFLOW.md):
- Orchestrator (principal agent)
- Explorer sub-agent
- Worker sub-agent
- Optimiser sub-agent

Follow that file when planning task delegation and execution responsibilities.

## Build & Validation Commands

```bash
# C++
mkdir -p build && cd build && cmake .. && make -j$(nproc)
cd build && make test

# Python
pip install -e .
pytest tests/unit/
```

## Development Rules

- Read nested `AGENTS.md` files before changing scoped directories.
- Follow [`docs/DEVELOPMENT_GUIDELINES.md`](docs/DEVELOPMENT_GUIDELINES.md).
- Track new work items in [`docs/TODOS.md`](docs/TODOS.md) with clear acceptance criteria.
- Keep secrets out of source control (`config/live/secrets.yaml` is gitignored).

## High-Level Layout

- `core/` — C++ hot path (orderbook, feeds, risk, execution, IPC, engine, shadow).
- `research/` — Python cold path (alpha, regime, backtesting utilities).
- `config/` — runtime and risk configuration by environment.
- `deploy/` — infrastructure and operational scripts.
- `docs/` — process docs, audits, and TODO tracking.
- `tests/` — unit, integration, replay, and performance tests.
