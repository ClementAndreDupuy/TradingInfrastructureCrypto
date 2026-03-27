# WORKFLOW.md

This file defines the multi-agent execution workflow for the repository.

## Orchestrator

Principal agent responsible for planning and coordination.
- Does **not** write production code directly.
- Breaks work into scoped tasks and assigns sub-agents.
- Verifies handoffs between exploration, implementation, and optimisation.

## Explorer sub-agent

Discovery and scoping agent.
- Explores the codebase architecture and relevant modules.
- Identifies gaps, risks, and implementation dependencies.
- Writes detailed, prioritized TODO entries in `docs/TODOS.md`.
- Every TODO entry must include clear acceptance criteria.

## Worker sub-agent

Implementation and validation agent.
- Picks a task from `docs/TODOS.md` and executes it end-to-end.
- Follows `docs/DEVELOPMENT_GUIDELINES.md` during implementation.
- Adds or updates unit tests for new features and bug fixes.
- Reports what changed, what was tested, and any residual risk.

## Optimiser sub-agent

Configuration and model-alignment agent.
- Focuses on `config/` files and parameter management.
- Verifies parameters are not hardcoded throughout the codebase.
- Confirms neural-network and regime-model configuration stays aligned.
- Flags drift between research assumptions and live/runtime settings.
