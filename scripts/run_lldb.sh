#!/bin/bash
# Geryon - A Distributed Shared Memory Framework
# Copyright (C) 2026 Zeeshan Qazi
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Run a specific test or test suite under LLDB for crash analysis.
# Usage: ./scripts/run_lldb.sh [gtest_filter]
# Example: ./scripts/run_lldb.sh NetworkSyncTest.BouncingOwnership
set -e

cd "$(dirname "$0")/.."

FILTER="${1:-NetworkSyncTest.BouncingOwnership}"
EXE="build/tests/geryon_tests"

if [ ! -f "$EXE" ]; then
    echo "Test binary not found at '$EXE'. Building first..."
    ./scripts/build.sh
fi

echo "Running under LLDB with filter: $FILTER"
lldb --batch \
    -o "run --gtest_filter=${FILTER}" \
    -o "bt all" \
    -o "quit" \
    "$EXE"
