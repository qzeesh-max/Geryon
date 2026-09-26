#!/bin/bash
docker exec $(docker ps -q) bash -c "apt-get update && apt-get install -y gdb && gdb -ex 'thread apply all bt' -batch -p \$(pidof geryon_tests) > /tmp/gdb_out.txt && cat /tmp/gdb_out.txt"
