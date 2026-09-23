#!/bin/bash
set -e

BOTTLE_NAME="$1"

if [ -z "$BOTTLE_NAME" ]; then
    echo "Usage: ./run_windows_tests_whisky.sh \"<BottleName>\""
    echo "Example: ./run_windows_tests_whisky.sh \"Windows 10\""
    exit 1
fi

WHISKY_CMD=""
if command -v whisky &> /dev/null; then
    WHISKY_CMD="whisky"
elif [ -f "/Applications/Whisky.app/Contents/Resources/WhiskyCmd" ]; then
    WHISKY_CMD="/Applications/Whisky.app/Contents/Resources/WhiskyCmd"
else
    echo "Whisky CLI could not be found."
    echo "Please install Whisky and the Whisky CLI (Whisky > Install Whisky CLI...)"
    exit 1
fi

EXE_PATH="$(pwd)/build-windows/tests/geryon_tests.exe"

if [ ! -f "$EXE_PATH" ]; then
    echo "Executable not found at $EXE_PATH"
    echo "Please run ./scripts/build_windows_on_mac.sh first."
    exit 1
fi

echo "Running Windows unit tests using Whisky in bottle: $BOTTLE_NAME"
"$WHISKY_CMD" run "$BOTTLE_NAME" "$EXE_PATH"
