#!/bin/bash
docker run --rm --shm-size=256m geryon-linux-tests bash -c "cd build-linux && apt-get update && apt-get install -y gdb && gdb -batch -ex 'run' -ex 'bt full' --args ./tests/geryon_tests --gtest_filter=CorrectnessTest.MultithreadMultipageCorrectness"
