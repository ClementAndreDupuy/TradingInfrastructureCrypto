__all__ = ["NeuralAlphaShadowSession", "ShadowSessionConfig"]


def __getattr__(name: str):
    if name in {"NeuralAlphaShadowSession", "ShadowSessionConfig"}:
        from .runtime.shadow_session import NeuralAlphaShadowSession, ShadowSessionConfig

        return {
            "NeuralAlphaShadowSession": NeuralAlphaShadowSession,
            "ShadowSessionConfig": ShadowSessionConfig,
        }[name]
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
