#!/bin/bash
# Geryon - A Distributed Shared Memory Framework
# Copyright (C) 2026 Zeeshan Qazi
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Build the project and run the native (macOS/Linux) test suite.
# Usage: ./scripts/run_tests.sh
set -e

cd "$(dirname "$0")/.."

./scripts/build.sh

cd build
ctest --output-on-failure
