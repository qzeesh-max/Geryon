#!/bin/bash
# Geryon - A Distributed Shared Memory Framework
# Copyright (C) 2026 Zeeshan Qazi
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Build and run the full test suite inside a Linux Docker container.
# Usage: ./scripts/run_linux_tests_docker.sh
set -e

cd "$(dirname "$0")/.."

if ! docker info &>/dev/null; then
    echo "Error: Docker daemon is not running."
    echo "Please start Docker Desktop and try again."
    exit 1
fi

echo "Building Docker image for Geryon Linux tests..."
docker build -t geryon-linux-tests .

echo "Running tests in Docker container..."
docker run --rm geryon-linux-tests
