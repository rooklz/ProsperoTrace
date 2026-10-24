#!/bin/sh
# run.sh — build and run all host-side checks:
#   check_trampoline.sh   emitter vs reference asm (byte-exact)
#   ring_decode_test      decoder: pairs, wraps, orphans
#   filter_test           hook-selection globs
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$ROOT/tests/check_trampoline.sh"

cc "$ROOT/tests/ring_decode_test.c" "$ROOT/agent/src/ring_decode.c" -o "$WORK/ring_decode" -Wall -Wextra
"$WORK/ring_decode"

cc "$ROOT/tests/filter_test.c" "$ROOT/agent/src/filter.c" -o "$WORK/filter" -Wall -Wextra
"$WORK/filter"

echo "all tests passed"