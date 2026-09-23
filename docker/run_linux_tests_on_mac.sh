#!/bin/bash
set -e
cd "$(dirname "$0")/.."

# Build the docker image
docker build -t geryon-linux-test -f docker/Dockerfile.ubuntu .

# Run the tests inside the container, mounting the current directory
docker run --rm -v "$(pwd):/workspace" geryon-linux-test
