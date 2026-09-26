#!/bin/bash
docker run --rm --shm-size=256m geryon-linux-tests bash -c "cd build-linux && ./tests/geryon_tests --gtest_filter=CorrectnessTest.MultithreadMultipageCorrectness"
