#!/usr/bin/env bash
# Unified local build script for C++ and Python components.
# Default behavior:
#   1) Build C++ engine with CMake
#   2) Install Python package in editable mode
#   3) Build pybind11 extension in-place
#   4) Byte-compile Python sources
#   5) Run unit tests

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_ROOT"

BUILD_CPP=1
BUILD_PYTHON=1
INSTALL_PYTHON=1
BUILD_BINDINGS=1
RUN_TESTS=1
RUN_CPP_TESTS=1
COMPILE_PYTHON=1
BUILD_DIR="build"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
PYTHON_BIN="${PYTHON_BIN:-}"

usage() {
    cat <<USAGE
Usage: ./scripts/build.sh [options]

Options:
  --skip-cpp              Skip C++ CMake build
  --skip-python           Skip all Python steps (install, bindings, compileall, tests)
  --skip-python-install   Skip 'python3 -m pip install -e .'
  --skip-bindings         Skip 'python3 bindings/setup.py build_ext --inplace'
  --skip-compileall       Skip Python byte-compilation check
  --skip-tests            Skip Python unit tests ('pytest tests/unit/')
  --skip-cpp-tests        Skip C++ tests ('ctest --output-on-failure')
  --build-dir <DIR>       CMake build directory (default: build)
  --jobs <N>              Parallel jobs for CMake build (default: detected CPU count)
  -h, --help              Show this help
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-cpp) BUILD_CPP=0; shift ;;
        --skip-python) BUILD_PYTHON=0; shift ;;
        --skip-python-install) INSTALL_PYTHON=0; shift ;;
        --skip-bindings) BUILD_BINDINGS=0; shift ;;
        --skip-compileall) COMPILE_PYTHON=0; shift ;;
        --skip-tests) RUN_TESTS=0; shift ;;
        --skip-cpp-tests) RUN_CPP_TESTS=0; shift ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --jobs) JOBS="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "[build] Unknown argument: $1"; usage; exit 1 ;;
    esac
done

if [[ "$BUILD_PYTHON" -eq 0 ]]; then
    INSTALL_PYTHON=0
    BUILD_BINDINGS=0
    RUN_TESTS=0
    COMPILE_PYTHON=0
fi

echo "=== ThamesRiverTrading local build ==="
echo "[build] project_root=$PROJECT_ROOT"

choose_python() {
    if [[ -n "$PYTHON_BIN" ]]; then
        return 0
    fi
    for candidate in python3.12 python3.11 python3.10 python3; do
        if command -v "$candidate" >/dev/null 2>&1; then
            if "$candidate" -c 'import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)' >/dev/null 2>&1; then
                PYTHON_BIN="$candidate"
                return 0
            fi
        fi
    done
    echo "[build] ERROR: Python 3.10+ is required, but no compatible interpreter was found on PATH."
    return 1
}

action_cpp() {
    echo "[build] Configuring CMake in $BUILD_DIR"
    cmake -S . -B "$BUILD_DIR"
    echo "[build] Building C++ targets (jobs=$JOBS)"
    cmake --build "$BUILD_DIR" -j "$JOBS"
}

action_python_install() {
    echo "[build] Installing Python package (editable)"
    if ! "$PYTHON_BIN" -m pip install -e .; then
        echo "[build] Editable install not supported, falling back to standard install"
        "$PYTHON_BIN" -m pip install .
    fi
}

action_bindings() {
    echo "[build] Building pybind11 bindings in-place"
    "$PYTHON_BIN" bindings/setup.py build_ext --inplace
}

action_compileall() {
    echo "[build] Byte-compiling Python sources"
    "$PYTHON_BIN" -m compileall research deploy tests
}

action_tests() {
    echo "[build] Running Python unit tests"
    "$PYTHON_BIN" -m pytest tests/unit/
}

action_cpp_tests() {
    echo "[build] Running C++ tests"
    ctest --test-dir "$BUILD_DIR" --output-on-failure
}

if [[ "$BUILD_CPP" -eq 1 ]]; then
    action_cpp
else
    echo "[build] Skipping C++ build"
fi

if [[ "$BUILD_PYTHON" -eq 1 ]]; then
    choose_python
    echo "[build] Using Python interpreter: $PYTHON_BIN ($("$PYTHON_BIN" --version 2>&1))"
fi

if [[ "$INSTALL_PYTHON" -eq 1 ]]; then
    action_python_install
else
    echo "[build] Skipping Python package install"
fi

if [[ "$BUILD_BINDINGS" -eq 1 ]]; then
    action_bindings
else
    echo "[build] Skipping pybind11 bindings build"
fi

if [[ "$COMPILE_PYTHON" -eq 1 ]]; then
    action_compileall
else
    echo "[build] Skipping Python byte-compilation"
fi

if [[ "$RUN_CPP_TESTS" -eq 1 && "$BUILD_CPP" -eq 1 ]]; then
    action_cpp_tests
else
    echo "[build] Skipping C++ tests"
fi

if [[ "$RUN_TESTS" -eq 1 ]]; then
    action_tests
else
    echo "[build] Skipping Python tests"
fi

echo "[build] Done."
