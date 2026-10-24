#include "ring_decode.h"

int ring_decode(struct pt_ring *r, pt_ring_pair_cb emit, void *ctx)
{
    uint32_t head = r->head;
    uint32_t tail = r->tail;
    int pairs = 0;

    while (tail != head) {
        uint32_t s = tail & PT_RING_MASK;
        uint64_t tag = r->slot[s].lane[0];
        uint32_t kind = pt_tag_kind(tag);
        uint32_t idx = pt_tag_index(tag);

        uint32_t s2 = (s + 1) & PT_RING_MASK;
        uint64_t tag2 = r->slot[s2].lane[0];

        if (kind == PT_TAG_ENTRY && pt_tag_kind(tag2) == PT_TAG_RET) {
            const struct pt_event *en = &r->slot[s];
            const struct pt_event *rt = &r->slot[s2];
            if (emit) {
                uint64_t args[6];
                for (int i = 0; i < 6; i++)
                    args[i] = en->lane[1 + i];
                emit(idx, args, rt->lane[7], ctx);
            }
            pairs++;
            tail += 2;      /* consume the whole pair */
        } else {
            tail += 1;      /* lone tag or torn lane-group: skip it */
        }
    }

    r->tail = tail;
    return pairs;
}