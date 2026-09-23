#!/bin/bash
# Geryon - A Distributed Shared Memory Framework
# Copyright (C) 2026 Zeeshan Qazi
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Build the project natively (macOS or Linux).
# Usage: ./scripts/build.sh [cmake-extra-args...]
set -e

# Change to project root regardless of where the script is called from
cd "$(dirname "$0")/.."

mkdir -p build
cd build

# On macOS, prefer Homebrew GCC over Apple Clang for better C++17/20 compatibility
if [ "$(uname)" == "Darwin" ]; then
    for version in 16 15 14 13; do
        if [ -f "/opt/homebrew/bin/g++-${version}" ]; then
            export CXX="/opt/homebrew/bin/g++-${version}"
            export CC="/opt/homebrew/bin/gcc-${version}"
            echo "Using Homebrew GCC ${version}: $CXX"
            break
        fi
    done
fi

cmake .. -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build . -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)"
echo "Build complete. Executables are in build/tests/ and build/benchmarks/."
