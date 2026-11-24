#!/bin/sh
set -e
cd "$(dirname "$0")"
gcc -O1 -g -Wall -Wextra -std=gnu11 -o /tmp/pt_native native_test.c ref_peer.c ../src/punter.c
timeout 600 /tmp/pt_native "$@"
