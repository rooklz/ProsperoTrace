#!/bin/sh
# run.sh — build and run all host-side checks:
#   check_trampoline.sh   emitter vs reference asm (byte-exact)
#   ring_decode_test      decoder: pairs, wraps, orphans
#   filter_test           hook-selection globs
#   jsonesc_test          JSON string escaping for event fields
#   conf_test             agent config file parsing
#   trampoline_exec_test  runs emitted trampolines (x86-64 native or Rosetta)
set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$ROOT/tests/check_trampoline.sh"

cc "$ROOT/tests/ring_decode_test.c" "$ROOT/agent/src/ring_decode.c" -o "$WORK/ring_decode" -Wall -Wextra
"$WORK/ring_decode"

cc "$ROOT/tests/filter_test.c" "$ROOT/agent/src/filter.c" -o "$WORK/filter" -Wall -Wextra
"$WORK/filter"

cc "$ROOT/tests/jsonesc_test.c" "$ROOT/agent/src/jsonesc.c" -o "$WORK/jsonesc" -Wall -Wextra
"$WORK/jsonesc"

cc "$ROOT/tests/conf_test.c" "$ROOT/agent/src/conf.c" -o "$WORK/conf" -Wall -Wextra
"$WORK/conf"

case "$(uname -m)" in
    x86_64) ARCH= ;;
    *) ARCH="-arch x86_64" ;;   # macOS on Apple silicon: run under Rosetta
esac
# shellcheck disable=SC2086
cc $ARCH -O1 "$ROOT/tests/trampoline_exec_test.c" "$ROOT/agent/src/trampoline.c" \
    "$ROOT/agent/src/ring_decode.c" -o "$WORK/tramp_exec" -Wall -Wextra
"$WORK/tramp_exec"

echo "all tests passed"