#!/bin/bash
set -e

# Check for MinGW-w64
if ! command -v x86_64-w64-mingw32-g++ &> /dev/null
then
    echo "x86_64-w64-mingw32-g++ could not be found."
    echo "Please install mingw-w64 to cross-compile for Windows:"
    echo "  brew install mingw-w64"
    exit 1
fi

echo "Building Geryon for Windows (x86_64) using MinGW-w64..."

mkdir -p build-windows
cd build-windows

# Configure using CMake with the MinGW toolchain
cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release -DBENCHMARK_ENABLE_WERROR=OFF

# Build the project
cmake --build . --config Release --parallel $(sysctl -n hw.ncpu)

echo "Build complete. Executables are in build-windows/tests and build-windows/benchmarks."
