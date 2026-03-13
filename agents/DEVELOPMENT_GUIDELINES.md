# DEVELOPMENT_GUIDELINES.md

1. C++ owns the hot path — Python never touches live execution or risk.
2. Order book integrity is foundational — all downstream depends on book correctness.
3. Shadow = live code path — identical code before going live.
4. Kill switch is non-negotiable — must work at OS/hardware level.
5. Never mock data — always use real data and real implementation.
6. Keep code clean and short — avoid unnecessary comments.
7. Always write production-ready code.
8. Run a full audit of the repo every week to identify bugs, grade the compared to industry standard and propose improvements
9. Always be 100% honest in your review 
10. Unit tests pass (`make test` + `pytest tests/unit/`)
11. Linters clean (`clang-format`, `black`, `ruff`)
12. No performance regression in C++, should always be faster
