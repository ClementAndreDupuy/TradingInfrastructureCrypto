"""
Build script for the trading_core pybind11 extension module.

Usage (from repo root):
    pip3 install pybind11 numpy
    python3 core/bindings/setup.py build_ext --inplace

The resulting trading_core.so lands in core/bindings/ and can be imported as:
    import sys; sys.path.insert(0, 'core/bindings')
    import trading_core

Prefer the CMake + scikit-build-core path for production builds:
    pip install scikit-build-core pybind11
    pip install -e . --no-build-isolation
"""

import os
import subprocess
import sys


def _abort(msg: str) -> None:
    print(f"\nERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def _pkg_config(package: str, flag: str) -> list[str]:
    try:
        out = subprocess.check_output(
            ["pkg-config", flag, package], stderr=subprocess.DEVNULL
        )
        return out.decode().strip().split()
    except FileNotFoundError:
        _abort("pkg-config not found. Install it: apt install pkg-config / brew install pkg-config")
    except subprocess.CalledProcessError:
        return []


def _require_pkg(package: str, install_hint: str) -> None:
    """Hard-fail if a pkg-config package is missing."""
    try:
        subprocess.check_call(
            ["pkg-config", "--exists", package],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except subprocess.CalledProcessError:
        _abort(
            f"Required library '{package}' not found by pkg-config.\n"
            f"  Install: {install_hint}\n"
            f"  If installed in a non-standard path, set PKG_CONFIG_PATH accordingly."
        )


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


# ── Dependency checks (hard failures) ────────────────────────────────────────

try:
    import pybind11
except ImportError:
    _abort("pybind11 not installed.\n  Fix: pip install pybind11")

_require_pkg(
    "libwebsockets",
    "apt install libwebsockets-dev  /  brew install libwebsockets",
)
_require_pkg(
    "libcurl",
    "apt install libcurl4-openssl-dev  /  brew install curl",
)
_require_pkg(
    "openssl",
    "apt install libssl-dev  /  brew install openssl@3",
)

# nlohmann_json is header-only and optional via pkg-config; missing is non-fatal
# if the header is reachable via the system include path.

# ── Paths ─────────────────────────────────────────────────────────────────────

from setuptools import Extension, setup  # noqa: E402

_here = os.path.dirname(os.path.abspath(__file__))
_root = os.path.dirname(os.path.dirname(_here))  # core/bindings -> core -> repo root
_core = os.path.join(_root, "core")

include_dirs: list[str] = [pybind11.get_include(), _core]
library_dirs: list[str] = []
libraries:    list[str] = []

# Release-quality flags; -std=c++17 is required — setuptools does not set it.
extra_compile_args: list[str] = ["-std=c++17", "-O2", "-fPIC", "-fvisibility=hidden", "-DNDEBUG"]
extra_link_args:    list[str] = []

# libwebsockets
_parse_flags(
    _pkg_config("libwebsockets", "--cflags-only-I")
    + _pkg_config("libwebsockets", "--libs"),
    include_dirs, library_dirs, libraries, extra_link_args,
)

# libcurl
_parse_flags(
    _pkg_config("libcurl", "--cflags-only-I") + _pkg_config("libcurl", "--libs"),
    include_dirs, library_dirs, libraries, extra_link_args,
)

# OpenSSL (libwebsockets transitively depends on ssl headers/libs)
_parse_flags(
    _pkg_config("openssl", "--cflags-only-I") + _pkg_config("openssl", "--libs"),
    include_dirs, library_dirs, libraries, extra_link_args,
)

# nlohmann/json (header-only — -I flags only)
for flag in _pkg_config("nlohmann_json", "--cflags-only-I"):
    if flag.startswith("-I"):
        include_dirs.append(flag[2:])


def _dedup(lst: list) -> list:
    seen: set = set()
    return [x for x in lst if not (x in seen or seen.add(x))]


include_dirs    = _dedup(include_dirs)
library_dirs    = _dedup(library_dirs)
libraries       = _dedup(libraries)
extra_link_args = _dedup(extra_link_args)

# ── Diagnostics ───────────────────────────────────────────────────────────────

print("trading_core build configuration:")
print(f"  pybind11 : {pybind11.__version__} @ {pybind11.get_include()}")
print(f"  include  : {include_dirs}")
print(f"  libs     : {libraries}")
print(f"  lib_dirs : {library_dirs}")
print(f"  cflags   : {extra_compile_args}")

# ── Extension ─────────────────────────────────────────────────────────────────

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
    python_requires=">=3.10",
    packages=[],
    ext_modules=[ext],
)
