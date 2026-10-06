#!/usr/bin/env bash
#
# Configure, build, and (optionally) test v6emul using the project-local .venv.
#
# Bootstraps the project virtual environment (.venv) with `uv` when available
# (falling back to `python3 -m venv`), then runs the CMake preset workflow. The
# environment's Python is placed on PATH so CMake and the ASM unit test runner
# pick it up automatically.
#
# Usage:
#   ./scripts/build.sh                 # configure + build (release)
#   ./scripts/build.sh -p ci -t        # configure + build + test (ci)
#   ./scripts/build.sh -p release -c   # clean, configure + build
#
set -euo pipefail

PRESET="release"
RUN_TESTS=0
CLEAN=0

usage() {
    echo "Usage: $0 [-p preset] [-t] [-c] [-h]"
    echo "  -p preset   CMake preset: debug, release, or ci (default: release)"
    echo "  -t          Run the CTest suite after building"
    echo "  -c          Clean the preset build directory before configuring"
    echo "  -h          Show this help"
}

while getopts "p:tch" opt; do
    case "$opt" in
        p) PRESET="$OPTARG" ;;
        t) RUN_TESTS=1 ;;
        c) CLEAN=1 ;;
        h) usage; exit 0 ;;
        *) usage; exit 1 ;;
    esac
done

case "$PRESET" in
    debug|release|ci) ;;
    *) echo "Unknown preset: $PRESET" >&2; usage; exit 1 ;;
esac

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

VENV_DIR="$ROOT/.venv"

# ── Ensure the project virtual environment exists ──────────────────────
if [ ! -x "$VENV_DIR/bin/python3" ] && [ ! -x "$VENV_DIR/bin/python" ]; then
    echo "Creating project virtual environment (.venv)..."
    if command -v uv >/dev/null 2>&1; then
        uv venv "$VENV_DIR"
    elif command -v python3 >/dev/null 2>&1; then
        python3 -m venv "$VENV_DIR"
    else
        echo "Cannot create .venv: install uv (https://docs.astral.sh/uv/) or Python 3.8+ first." >&2
        exit 1
    fi
fi

# Make the venv interpreter the default `python` for the whole build.
# shellcheck disable=SC1091
if [ -f "$VENV_DIR/bin/activate" ]; then
    . "$VENV_DIR/bin/activate"
fi
echo "Using Python: $(python3 --version 2>&1 || python --version 2>&1)"

# ── Optional clean ─────────────────────────────────────────────────────
BUILD_DIR="$ROOT/build/$PRESET"
if [ "$CLEAN" -eq 1 ] && [ -d "$BUILD_DIR" ]; then
    echo "Removing $BUILD_DIR..."
    rm -rf "$BUILD_DIR"
fi

# ── Configure & build ──────────────────────────────────────────────────
cmake --preset "$PRESET"
cmake --build --preset "$PRESET"

# ── Test ───────────────────────────────────────────────────────────────
if [ "$RUN_TESTS" -eq 1 ]; then
    if [ "$PRESET" = "debug" ]; then
        CONFIG="Debug"
    else
        CONFIG="Release"
    fi
    ctest --test-dir "$BUILD_DIR" --build-config "$CONFIG" --output-on-failure
fi
