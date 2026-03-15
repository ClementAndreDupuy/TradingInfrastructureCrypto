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
COMPILE_PYTHON=1
BUILD_DIR="build"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

usage() {
    cat <<USAGE
Usage: ./scripts/build.sh [options]

Options:
  --skip-cpp              Skip C++ CMake build
  --skip-python           Skip all Python steps (install, bindings, compileall, tests)
  --skip-python-install   Skip 'python3 -m pip install -e .'
  --skip-bindings         Skip 'python3 bindings/setup.py build_ext --inplace'
  --skip-compileall       Skip Python byte-compilation check
  --skip-tests            Skip 'pytest tests/unit/'
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

action_cpp() {
    echo "[build] Configuring CMake in $BUILD_DIR"
    cmake -S . -B "$BUILD_DIR"
    echo "[build] Building C++ targets (jobs=$JOBS)"
    cmake --build "$BUILD_DIR" -j "$JOBS"
}

action_python_install() {
    echo "[build] Installing Python package (editable)"
    python3 -m pip install -e .
}

action_bindings() {
    echo "[build] Building pybind11 bindings in-place"
    python3 bindings/setup.py build_ext --inplace
}

action_compileall() {
    echo "[build] Byte-compiling Python sources"
    python3 -m compileall research deploy tests
}

action_tests() {
    echo "[build] Running Python unit tests"
    pytest tests/unit/
}

if [[ "$BUILD_CPP" -eq 1 ]]; then
    action_cpp
else
    echo "[build] Skipping C++ build"
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

if [[ "$RUN_TESTS" -eq 1 ]]; then
    action_tests
else
    echo "[build] Skipping tests"
fi

echo "[build] Done."
