#!/usr/bin/env bash
# ChatLink - link a TI-Nspire CX to a PC over USB.
# Copyright (C) 2026 Christopher Willett / AnvilDevelopment.US
# SPDX-License-Identifier: GPL-3.0-or-later

# Configures and builds ChatLink using the toolchain CLion bundles.
#
# CLion ships CMake, Ninja and MinGW-w64 but does not put them on PATH, and gcc
# needs its own bin directory on PATH to find `as` and `ld`. Passing the
# compiler paths to CMake alone is not enough, hence both here.
#
# Usage: tools/build.sh [Debug|Release]   (default Debug)

set -euo pipefail

BUILD_TYPE="${1:-Debug}"
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

# Pick the newest CLion install rather than pinning a version.
CLION_BIN="$(ls -d "/c/Program Files/JetBrains/CLion "*/bin 2>/dev/null | sort -V | tail -1)"
if [[ -z "$CLION_BIN" ]]; then
    echo "error: no CLion installation found under C:/Program Files/JetBrains" >&2
    exit 1
fi

CMAKE="$CLION_BIN/cmake/win/x64/bin/cmake.exe"
NINJA="$CLION_BIN/ninja/win/x64/ninja.exe"
MINGW_BIN="$CLION_BIN/mingw/bin"

for tool in "$CMAKE" "$NINJA" "$MINGW_BIN/gcc.exe" "$MINGW_BIN/g++.exe"; do
    [[ -f "$tool" ]] || { echo "error: missing $tool" >&2; exit 1; }
done

export PATH="$MINGW_BIN:$PATH"

"$CMAKE" -S "$PROJECT_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$NINJA" \
    -DCMAKE_C_COMPILER="$MINGW_BIN/gcc.exe" \
    -DCMAKE_CXX_COMPILER="$MINGW_BIN/g++.exe" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"

"$CMAKE" --build "$BUILD_DIR" --parallel

echo
echo "Built: $BUILD_DIR/bin/ChatLink.exe"
