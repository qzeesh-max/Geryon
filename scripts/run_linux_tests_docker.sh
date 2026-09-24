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
# userfaultfd requires either --privileged or seccomp=unconfined.
# Docker's default seccomp profile does not whitelist the userfaultfd(2) syscall,
# which Geryon uses on Linux for signal-safe page fault interception.
# We use seccomp=unconfined as the portable option; in production environments
# a tailored seccomp profile whitelisting only userfaultfd should be used instead.
docker run --rm --shm-size=256m \
    --security-opt seccomp=unconfined \
    --cap-add SYS_PTRACE \
    geryon-linux-tests
