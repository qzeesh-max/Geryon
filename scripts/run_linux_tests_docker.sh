#!/bin/bash
set -e

echo "Building Docker image for Geryon Linux tests..."
docker build -t geryon-linux-tests .

echo "Running tests in Docker container..."
docker run --rm geryon-linux-tests
