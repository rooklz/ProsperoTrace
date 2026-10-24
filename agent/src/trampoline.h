#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Per-hook trampoline emitter.
 *
 * Each hook gets a small block of machine code that:
 *   - saves the six SysV argument registers
 *   - records an entry event in the shared ring (tag, args)
 *   - calls the original function
 *   - records a ret event (tag, return value)
 *   - tail-jumps to the original
 *
 * The template is a fixed byte sequence (see tests/trampoline_ref.s, the
 * authoritative encoding); absolute addresses are placed by scanning for
 * unique placeholder immediates and rewriting them. Maximum template size
 * is PT_TRAMPOLINE_MAX.
 */

#define PT_TRAMPOLINE_MAX 320u

int trampoline_emit(uint64_t ring_va, uint64_t orig_va, uint32_t hook_index,
                    uint8_t *out, size_t cap, size_t *out_len);