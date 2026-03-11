"""Neural alpha pipeline: LOB + tick data → GNN/Transformer → multi-task alpha."""

from .shadow_session import NeuralAlphaShadowSession, ShadowSessionConfig

__all__ = ["NeuralAlphaShadowSession", "ShadowSessionConfig"]
