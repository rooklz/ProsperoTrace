#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Shared ring between the target (producer) and the tracer (consumer).
 *
 * The trampolines emitted by trampoline.c write events here using plain
 * stores; the agent's reader thread drains the ring and converts each event
 * to the JSONL stream. No locks: inserts are a single non-atomic head bump;
 * readers only observe slots behind `head` and never rewind. See
 * docs/DESIGN.md for the single-writer caveats and the M4 seqlock plan.
 */

#define PT_RING_CAP_EVENTS  63u            /* events per ring (power of two) */
#define PT_RING_MASK        (PT_RING_CAP_EVENTS - 1u)
#define PT_EVENT_LANES      8u             /* u64 lanes per event */
#define PT_EVENT_BYTES      (PT_EVENT_LANES * 8u)

#define PT_TAG_ENTRY        1u
#define PT_TAG_RET          2u

struct pt_event {
    uint64_t lane[PT_EVENT_LANES];
};

/* header 16 bytes, then rings of capped events */
struct pt_ring {
    uint32_t head;                        /* +0  bumped by target */
    uint32_t tail;                        /* +4  advanced by reader */
    uint32_t dropped;                     /* +8  target-side drops (unused in v1) */
    uint32_t pad;                         /* +12 */
    struct pt_event slot[PT_RING_CAP_EVENTS];
};

#define PT_RING_SIZE (sizeof(struct pt_ring))

/* lane0 encodings */
static inline uint64_t pt_make_tag(uint32_t kind, uint32_t hook_index)
{
    return ((uint64_t)kind << 32) | hook_index;
}
static inline uint32_t pt_tag_kind(uint64_t tag)
{
    return (uint32_t)(tag >> 32);
}
static inline uint32_t pt_tag_index(uint64_t tag)
{
    return (uint32_t)tag;
}