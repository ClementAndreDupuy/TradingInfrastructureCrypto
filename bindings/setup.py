"""
Build the perp_arb_sim C++ extension module.

Usage (from repo root):
    pip3 install pybind11
    python3 bindings/setup.py build_ext --inplace

The compiled .so will land in bindings/ and is auto-discovered when
perp_arb_backtest.py is run from the repo root.
"""
import os
import sys
from pathlib import Path

from setuptools import Extension, setup

try:
    import pybind11
except ImportError:
    raise SystemExit("pybind11 not found. Run: pip3 install pybind11") from None

ROOT = Path(__file__).parent.parent.resolve()

ext = Extension(
    name="perp_arb_sim",
    sources=[str(Path(__file__).parent / "perp_arb_bindings.cpp")],
    include_dirs=[
        pybind11.get_include(),
        str(ROOT),          # so #include "core/..." resolves
    ],
    extra_compile_args=[
        "-std=c++17",
        "-O3",
        "-DNDEBUG",
        "-fvisibility=hidden",
    ],
    language="c++",
)

setup(
    name="perp_arb_sim",
    version="0.1.0",
    description="Perp-arb C++ simulation engine (pybind11 bridge)",
    ext_modules=[ext],
    zip_safe=False,
)