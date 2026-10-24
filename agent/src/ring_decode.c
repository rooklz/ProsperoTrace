#include "ring_decode.h"

int ring_decode(struct pt_ring *r, pt_ring_pair_cb emit, void *ctx, int skip_inflight)
{
    uint32_t head = r->head & ~1u;
    uint32_t tail = r->tail & ~1u;
    int pairs = 0;

    if (head - tail > PT_RING_CAP_EVENTS) {
        /* the target lapped us: everything older than one ring is gone */
        r->dropped += (head - tail - PT_RING_CAP_EVENTS) / 2;
        tail = head - PT_RING_CAP_EVENTS;
    }

    while (tail != head) {
        uint32_t s = tail & PT_RING_MASK;
        const struct pt_event *en = &r->slot[s];
        const struct pt_event *rt = &r->slot[s + 1];
        uint64_t tag = en->lane[0];
        uint64_t tag2 = rt->lane[0];

        if (pt_tag_kind(tag) == PT_TAG_ENTRY && tag2 == 0 && !skip_inflight)
            break;          /* call still running: wait for its ret */

        if (pt_tag_kind(tag) == PT_TAG_ENTRY && pt_tag_kind(tag2) == PT_TAG_RET &&
            pt_tag_index(tag) == pt_tag_index(tag2)) {
            if (emit) {
                uint64_t args[6];
                for (int i = 0; i < 6; i++)
                    args[i] = en->lane[1 + i];
                emit(pt_tag_index(tag), args, rt->lane[7], ctx);
            }
            pairs++;
        }
        tail += 2;          /* pairs sit on even slots; junk is skipped whole */
        if (skip_inflight)
            skip_inflight = 0;  /* only the oldest stalled call is given up */
    }

    r->tail = tail;
    return pairs;
}
