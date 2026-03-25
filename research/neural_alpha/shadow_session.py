"""Shim so that `python -m research.neural_alpha.shadow_session` works.

The implementation lives in research.neural_alpha.runtime.shadow_session.
"""
from research.neural_alpha.runtime.shadow_session import (
    NeuralAlphaShadowSession,
    ShadowSessionConfig,
)

if __name__ == "__main__":
    from research.neural_alpha.runtime.shadow_session import main

    main()
