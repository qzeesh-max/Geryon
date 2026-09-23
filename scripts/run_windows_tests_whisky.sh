#!/bin/bash
# Geryon - A Distributed Shared Memory Framework
# Copyright (C) 2026 Zeeshan Qazi
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Run the Windows test suite using Whisky on macOS.
# Builds the Windows executable first if it doesn't exist.
#
# Usage: ./scripts/run_windows_tests_whisky.sh ["<BottleName>"] [extra-gtest-args...]
# Example: ./scripts/run_windows_tests_whisky.sh "Windows 10"
set -e

cd "$(dirname "$0")/.."

BOTTLE_NAME="${1:-Windows 10}"
shift || true   # Consume the bottle name; remaining args passed to the test binary

WHISKY_CMD=""
if command -v whisky &>/dev/null; then
    WHISKY_CMD="whisky"
elif [ -f "/Applications/Whisky.app/Contents/Resources/WhiskyCmd" ]; then
    WHISKY_CMD="/Applications/Whisky.app/Contents/Resources/WhiskyCmd"
else
    echo "Error: Whisky CLI could not be found."
    echo "Please install Whisky and the Whisky CLI via: Whisky > Install Whisky CLI..."
    exit 1
fi

EXE_PATH="$(pwd)/build-windows/tests/geryon_tests.exe"

if [ ! -f "$EXE_PATH" ]; then
    echo "Windows executable not found. Building first..."
    ./scripts/build_windows_on_mac.sh
fi

echo "Running Windows test suite via Whisky (bottle: '$BOTTLE_NAME')..."
"$WHISKY_CMD" run "$BOTTLE_NAME" "$EXE_PATH" "$@"
