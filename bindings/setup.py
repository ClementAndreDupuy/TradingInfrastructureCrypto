"""
Build script for the trading_core pybind11 extension module.

Usage (from repo root):
    pip3 install pybind11
    python3 bindings/setup.py build_ext --inplace

The resulting trading_core.so lands in bindings/ and can be imported as:
    import sys; sys.path.insert(0, 'bindings')
    import trading_core
"""

import os
import subprocess
import sys

from setuptools import Extension, setup


def _pkg_config(package: str, flag: str) -> list[str]:
    try:
        out = subprocess.check_output(
            ["pkg-config", flag, package], stderr=subprocess.DEVNULL
        )
        return out.decode().strip().split()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return []


def _parse_flags(raw: list[str], include_dirs, library_dirs, libraries, extra_link):
    for f in raw:
        if f.startswith("-I"):
            include_dirs.append(f[2:])
        elif f.startswith("-L"):
            library_dirs.append(f[2:])
        elif f.startswith("-l"):
            libraries.append(f[2:])
        elif f:
            extra_link.append(f)


import pybind11  # noqa: E402  (must be installed first)

# Paths
_here = os.path.dirname(os.path.abspath(__file__))
_root = os.path.dirname(_here)
_core = os.path.join(_root, "core")

include_dirs: list[str] = [pybind11.get_include(), _core]
library_dirs: list[str] = []
libraries: list[str] = []
extra_compile_args: list[str] = ["-std=c++17", "-O2", "-fvisibility=hidden"]
extra_link_args: list[str] = []

# libwebsockets
_parse_flags(
    _pkg_config("libwebsockets", "--cflags-only-I")
    + _pkg_config("libwebsockets", "--libs"),
    include_dirs, library_dirs, libraries, extra_link_args,
)

# libcurl
_parse_flags(
    _pkg_config("libcurl", "--cflags-only-I")
    + _pkg_config("libcurl", "--libs"),
    include_dirs, library_dirs, libraries, extra_link_args,
)

# nlohmann/json (header-only — only need -I)
for flag in _pkg_config("nlohmann_json", "--cflags-only-I"):
    if flag.startswith("-I"):
        include_dirs.append(flag[2:])

# Deduplicate while preserving order
def _dedup(lst: list) -> list:
    seen: set = set()
    return [x for x in lst if not (x in seen or seen.add(x))]

include_dirs  = _dedup(include_dirs)
library_dirs  = _dedup(library_dirs)
libraries     = _dedup(libraries)
extra_link_args = _dedup(extra_link_args)

ext = Extension(
    name="trading_core",
    sources=[
        os.path.join(_here, "bindings.cpp"),
        os.path.join(_core, "feeds", "binance", "binance_feed_handler.cpp"),
        os.path.join(_core, "feeds", "kraken", "kraken_feed_handler.cpp"),
    ],
    include_dirs=include_dirs,
    library_dirs=library_dirs,
    libraries=libraries,
    extra_compile_args=extra_compile_args,
    extra_link_args=extra_link_args,
    language="c++",
)

setup(
    name="trading-core",
    version="1.0.0",
    description="ThamesRiverTrading C++ pybind11 bridge",
    ext_modules=[ext],
)
