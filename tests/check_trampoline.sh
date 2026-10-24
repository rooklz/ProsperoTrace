#!/bin/sh
# check_trampoline.sh — asserts the C emitter (trampoline.c) produces exactly
# the byte sequence of the reference assembly (trampoline_ref.s).
#
# This catches any drift between the documented instruction sequence and the
# template embedded in the emitter. Run from the repo root:
#
#   path/to/tests/check_trampoline.sh
#
# Requires a clang with the x86_64 backend (any host) and LLVM objdump.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

clang --target=x86_64-apple-macosx -c "$ROOT/tests/trampoline_ref.s" -o "$WORK/ref.o"
# objdump -s: <addr> <4x8hex> <ascii> -> keep only the hex columns
objdump -s -j __text "$WORK/ref.o" \
    | awk '/^ +[0-9a-fA-F]{4} / { printf "%s%s%s%s", $2, $3, $4, $5 }' \
    | xxd -r -p > "$WORK/ref.bin"

clang "$ROOT/tests/dump_template.c" "$ROOT/agent/src/trampoline.c" -o "$WORK/dump"
"$WORK/dump" > "$WORK/emit.bin"

if cmp -s "$WORK/ref.bin" "$WORK/emit.bin"; then
    echo "OK: emitter matches reference ($(wc -c < "$WORK/emit.bin") bytes)"
else
    echo "FAIL: emitter and reference differ" >&2
    cmp -l "$WORK/ref.bin" "$WORK/emit.bin" | head -20 >&2 || true
    echo "ref: $(wc -c < "$WORK/ref.bin" | tr -d ' ') bytes; emit: $(wc -c < "$WORK/emit.bin" | tr -d ' ') bytes" >&2
    exit 1
fi