#pragma once

#include "ring.h"

#include <stdint.h>

/*
 * Ring decoder: turns raw target-written events into completed (args, ret)
 * pairs. Pure and dependency-free so it runs identically on the console and
 * in host tests (tests/ring_decode_test.c).
 *
 * Protocol (enforced by trampoline.c): head advances atomically by two per
 * call, so each call owns an even slot (ENTRY) and the next one (RET). The
 * RET tag is zero while the call runs; decoding stops there so blocking
 * calls are not lost. Junk pairs are skipped, and if the target laps the
 * reader the overwritten pairs are counted in r->dropped.
 */

typedef void (*pt_ring_pair_cb)(uint32_t hook_index, const uint64_t args[6],
                                uint64_t ret, void *ctx);

/* Drain completed pairs from r and advance r->tail. Returns pair count.
 * skip_inflight gives up on the oldest still-running call (the caller
 * decides when one has stalled for too long). */
int ring_decode(struct pt_ring *r, pt_ring_pair_cb emit, void *ctx, int skip_inflight);