#!/bin/bash
lldb --batch -o "run --gtest_filter=NetworkSyncTest.BouncingOwnership" -o "bt all" -o "quit" build/tests/geryon_tests
