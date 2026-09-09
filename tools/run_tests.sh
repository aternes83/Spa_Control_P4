#!/usr/bin/env bash
# All host tests. No hardware, no ESP-IDF, no MicroPython — just python3 and a C
# compiler. Run this before every commit.
set -u
cd "$(dirname "$0")/.."

BUILD="$(mktemp -d)"
trap 'rm -rf "$BUILD"' EXIT
CC="${CC:-cc}"
fail=0

run() {
    echo
    echo "── $1 ──────────────────────────────────────────────────────"
    shift
    "$@" || fail=1
}

run "spalink codec (C vs Python)"  python3 tools/test_spalink.py
run "control core + differential"  python3 tools/test_spa_core.py
run "link session"                 python3 tools/test_link_session.py

echo
echo "── HMI state model (C) ─────────────────────────────────────"
if "$CC" -std=c11 -Wall -Wextra -Werror -I link -o "$BUILD/test_spa_state" \
        tools/test_spa_state.c p4_hmi/main/spa_state.c link/spalink_codec.c; then
    "$BUILD/test_spa_state" || fail=1
else
    echo "  FAIL could not compile the state-model tests"
    fail=1
fi

echo
if [ "$fail" -ne 0 ]; then
    echo "SOME TESTS FAILED"
    exit 1
fi
echo "ALL SUITES PASSED"
