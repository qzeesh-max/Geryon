#!/bin/bash
# Geryon - A Distributed Shared Memory Framework
# Copyright (C) 2026 Zeeshan Qazi
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Cross-compile Geryon for Windows (x86_64) using MinGW-w64 on macOS/Linux.
# Usage: ./scripts/build_windows_on_mac.sh
set -e

cd "$(dirname "$0")/.."

# Check for MinGW-w64
if ! command -v x86_64-w64-mingw32-g++ &>/dev/null; then
    echo "Error: x86_64-w64-mingw32-g++ could not be found."
    echo "Please install mingw-w64:"
    echo "  brew install mingw-w64"
    exit 1
fi

echo "Building Geryon for Windows (x86_64) using MinGW-w64..."

mkdir -p build-windows
cd build-windows

cmake .. \
    -DCMAKE_TOOLCHAIN_FILE=../cmake/mingw-w64-x86_64.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DBENCHMARK_ENABLE_WERROR=OFF

cmake --build . --config Release --parallel "$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

echo "Build complete. Executables are in build-windows/tests/ and build-windows/benchmarks/."
