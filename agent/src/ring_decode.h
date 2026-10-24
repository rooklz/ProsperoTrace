#pragma once

#include "ring.h"

#include <stdint.h>

/*
 * Ring decoder: turns raw target-written events into completed (args, ret)
 * pairs. Pure and dependency-free so it runs identically on the console and
 * in host tests (tests/ring_decode_test.c).
 *
 * Protocol (enforced by trampoline.c): a call is an ENTRY lane-group
 * immediately followed by a RET lane-group in ring order; head advances by
 * two per call, so every pair is visible together. Lone or misordered
 * lane-groups are skipped defensively.
 */

typedef void (*pt_ring_pair_cb)(uint32_t hook_index, const uint64_t args[6],
                                uint64_t ret, void *ctx);

/* Drain all completed pairs from r and advance r->tail. Returns pair count. */
int ring_decode(struct pt_ring *r, pt_ring_pair_cb emit, void *ctx);