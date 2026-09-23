#!/bin/bash
# Geryon - A Distributed Shared Memory Framework
# Copyright (C) 2026 Zeeshan Qazi
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Run the Windows test suite using CrossOver on macOS.
# Builds the Windows executable first if it doesn't exist.
#
# Usage: ./scripts/run_windows_tests_crossover.sh ["<BottleName>"] [extra-gtest-args...]
# Example: ./scripts/run_windows_tests_crossover.sh "Windows 10"
# Example: ./scripts/run_windows_tests_crossover.sh "Windows 10" --gtest_filter=NetworkSyncTest.*
set -e

cd "$(dirname "$0")/.."

BOTTLE_NAME="${1:-Windows 10}"
shift || true   # Consume the bottle name; remaining args passed to the test binary

CROSSOVER_WINE="/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine"

if [ ! -f "$CROSSOVER_WINE" ]; then
    echo "Error: CrossOver wine executable not found at:"
    echo "  $CROSSOVER_WINE"
    echo "Please ensure CrossOver is installed in /Applications/CrossOver.app"
    exit 1
fi

EXE_PATH="$(pwd)/build-windows/tests/geryon_tests.exe"

if [ ! -f "$EXE_PATH" ]; then
    echo "Windows executable not found. Building first..."
    ./scripts/build_windows_on_mac.sh
fi

echo "Running Windows test suite via CrossOver (bottle: '$BOTTLE_NAME')..."
"$CROSSOVER_WINE" --bottle "$BOTTLE_NAME" --cx-app "$EXE_PATH" "$@"
