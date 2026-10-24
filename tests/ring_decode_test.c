/* ring_decode_test.c — host test for the target→agent event decoder.
 * Builds a synthetic ring, runs ring_decode, and asserts the emitted pairs. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../agent/src/ring.h"
#include "../agent/src/ring_decode.h"

struct sink {
    uint32_t pairs;
    uint32_t idx[16];
    uint64_t args[16][6];
    uint64_t ret[16];
};

static void sink(uint32_t hook_index, const uint64_t args[6], uint64_t ret, void *ctx)
{
    struct sink *s = ctx;
    assert(s->pairs < 16);
    uint32_t n = s->pairs++;
    s->idx[n] = hook_index;
    memcpy(s->args[n], args, 6 * sizeof(uint64_t));
    s->ret[n] = ret;
}

static struct pt_ring *new_ring(void)
{
    struct pt_ring *r = calloc(1, sizeof(*r));
    assert(r);
    return r;
}

static void put_pair(struct pt_ring *r, uint32_t hook_index,
                     const uint64_t args[6], uint64_t ret)
{
    uint32_t s = r->head & PT_RING_MASK;
    struct pt_event *en = &r->slot[s];
    struct pt_event *rt = &r->slot[(s + 1) & PT_RING_MASK];

    memset(en, 0, sizeof(*en));
    memset(rt, 0, sizeof(*rt));
    en->lane[0] = pt_make_tag(PT_TAG_ENTRY, hook_index);
    for (int i = 0; i < 6; i++)
        en->lane[1 + i] = args[i];
    rt->lane[0] = pt_make_tag(PT_TAG_RET, hook_index);
    rt->lane[7] = ret;
    r->head += 2;   /* matches the trampoline's single head bump */
}

int main(void)
{
    struct pt_ring *r = new_ring();

    /* 1: two ordinary pairs */
    uint64_t a1[6] = { 0x100, 0x200, 0x300, 0x400, 0x500, 0x600 };
    uint64_t a2[6] = { 0x1, 0x2, 0x3, 0x4, 0x5, 0x6 };
    put_pair(r, 3, a1, 0xdeadbeef);
    put_pair(r, 7, a2, 0x1);

    struct sink s = { 0 };
    assert(ring_decode(r, sink, &s) == 2);
    assert(s.idx[0] == 3 && s.ret[0] == 0xdeadbeef);
    assert(memcmp(s.args[0], a1, sizeof(a1)) == 0);
    assert(s.idx[1] == 7 && s.ret[1] == 0x1);
    assert(memcmp(s.args[1], a2, sizeof(a2)) == 0);

    /* 2: torn/garbage lane-groups are reclaimed, never fabricated into pairs */
    uint32_t head_after = r->head + 2;              /* slots 4,5 never written */
    r->head = head_after;
    assert(ring_decode(r, sink, &s) == 0);
    assert(r->tail == r->head);                     /* ring does not wedge */

    /* 3: wrap-around at the mask boundary (slot 63 + slot 0) */
    memset(r, 0, sizeof(*r));
    r->head = PT_RING_MASK & ~1u;                   /* even, last position */
    r->tail = r->head;
    uint64_t aw[6] = { 9, 9, 9, 9, 9, 9 };
    put_pair(r, 11, aw, 0xabcdef);
    struct sink s2 = { 0 };
    assert(ring_decode(r, sink, &s2) == 1);
    assert(s2.idx[0] == 11 && s2.ret[0] == 0xabcdef);
    assert(r->tail == ((PT_RING_MASK & ~1u) + 2));  /* head is back to even */

    /* 4: orphan RET and lone ENTRY are skipped, ring keeps advancing */
    struct pt_ring *r3 = new_ring();
    r3->slot[0].lane[0] = pt_make_tag(PT_TAG_RET, 5);          /* orphan      */
    r3->slot[1].lane[0] = pt_make_tag(PT_TAG_ENTRY, 9);        /* no ret pair */
    r3->slot[2].lane[0] = pt_make_tag(0, 0);                   /* junk        */
    r3->head = 6;
    struct sink s3 = { 0 };
    assert(ring_decode(r3, sink, &s3) == 0);

    puts("ring_decode_test: OK");
    return 0;
}