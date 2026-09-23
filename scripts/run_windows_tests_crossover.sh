#!/bin/bash
set -e

BOTTLE_NAME="$1"

if [ -z "$BOTTLE_NAME" ]; then
    echo "Usage: ./run_windows_tests_crossover.sh \"<BottleName>\""
    echo "Example: ./run_windows_tests_crossover.sh \"Windows 10\""
    exit 1
fi

CROSSOVER_CMD="/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine"

if [ ! -f "$CROSSOVER_CMD" ]; then
    echo "CrossOver wine executable could not be found."
    echo "Please ensure CrossOver is installed in /Applications/CrossOver.app"
    exit 1
fi

EXE_PATH="${2:-$(pwd)/build-windows/tests/geryon_tests.exe}"

if [ ! -f "$EXE_PATH" ]; then
    echo "Executable not found at $EXE_PATH"
    echo "Please run ./scripts/build_windows_on_mac.sh first."
    exit 1
fi

echo "Running Windows executable using CrossOver in bottle: $BOTTLE_NAME"
"$CROSSOVER_CMD" --bottle "$BOTTLE_NAME" "$EXE_PATH"
