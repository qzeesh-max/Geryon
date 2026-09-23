#!/bin/bash
set -e

mkdir -p build
cd build

if [ "$(uname)" == "Darwin" ]; then
    if [ -f "/opt/homebrew/bin/g++-16" ]; then
        export CXX=/opt/homebrew/bin/g++-16
        export CC=/opt/homebrew/bin/gcc-16
    fi
fi

cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)
