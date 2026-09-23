#!/bin/bash
set -e
cd "$(dirname "$0")/.."
./scripts/build.sh
cd build
ctest --output-on-failure
