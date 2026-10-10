#!/bin/sh
# Builds and runs the host-side tests (no ESP-IDF needed). Any C compiler.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
BT="$HERE/../../components/bt_companion"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/redial_host_tests.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
# Built from a copy in a scratch directory: on the machine this was written
# on, a binary compiled straight from the repo tree hung at launch (stuck
# exiting, no output) while a byte-identical-source build from a scratch copy
# ran fine. Cause not found; this sidesteps it.
cp "$BT/include/bk3266_proto.h" "$BT/bk3266_proto.c" "$HERE"/test_*.c "$OUT/"
cd "$OUT"
for t in test_bk3266_proto; do
    cc -std=c11 -Wall -Wextra -Werror -O1 -I. "$t.c" bk3266_proto.c -o "run_$t"
    "./run_$t"
done
