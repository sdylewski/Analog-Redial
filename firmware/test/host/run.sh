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
DT="$HERE/../../components/dtmf"
cp "$BT/include/bt401_proto.h" "$BT/bt401_proto.c" "$HERE"/test_*.c "$OUT/"
cp -r "$HERE/shim" "$OUT/"
cp "$DT/dtmf.c" "$DT/dtmf_test.c" "$DT/include/dtmf.h" "$DT/include/dtmf_test.h" "$HERE/../../components/tones/include/tones.h" "$OUT/"
cd "$OUT"
for t in test_bt401_proto; do
    cc -std=c11 -Wall -Wextra -Werror -O1 -I. "$t.c" bt401_proto.c -o "run_$t"
    "./run_$t"
done
cc -std=gnu11 -Wall -Wextra -O1 -Ishim -I. test_dtmf.c dtmf.c -lm -o run_test_dtmf
./run_test_dtmf
cc -std=gnu11 -Wall -Wextra -O1 -Ishim -I. test_dtmf_test.c dtmf_test.c dtmf.c -lm -o run_test_dtmf_test
./run_test_dtmf_test
PP="$HERE/../../components/payphone"
cp "$PP/coin_tone.c" "$PP/include/coin_tone.h" "$OUT/"
cc -std=gnu11 -Wall -Wextra -O1 -I. test_coin_tone.c coin_tone.c -lm -o run_test_coin_tone
./run_test_coin_tone
EX="$HERE/../../components/ag1170"
cp "$EX/line_expander.c" "$EX/include/line_expander.h" "$OUT/"
cp -r "$HERE/shim_line_expander" "$OUT/"
cc -std=gnu11 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -O1 -Ishim_line_expander -I. \
   test_line_expander.c -o run_test_line_expander
./run_test_line_expander
