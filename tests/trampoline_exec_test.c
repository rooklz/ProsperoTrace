/* trampoline_exec_test.c — run emitted trampolines on x86-64 (native or
 * Rosetta) and check the callee sees the caller's exact arguments, the
 * caller gets the callee's exact result, and the ring holds the pair. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "../agent/src/ring.h"
#include "../agent/src/ring_decode.h"
#include "../agent/src/trampoline.h"

static long nine(long a, long b, long c, long d, long e, long f, long g, long h, long i)
{
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i;
}

static double vsum(int n, ...)
{
    va_list ap;
    double t = 0;
    va_start(ap, n);
    for (int k = 0; k < n; k++)
        t += va_arg(ap, double);
    va_end(ap);
    return t;
}

struct pair128 { long lo, hi; };
static struct pair128 wide(long x) { struct pair128 p = { x, ~x }; return p; }

static struct pt_ring ring __attribute__((aligned(64)));

struct got { int n; uint32_t idx[8]; uint64_t a0[8], ret[8]; };
static void sink(uint32_t i, const uint64_t a[6], uint64_t r, void *ctx)
{
    struct got *g = ctx;
    g->idx[g->n] = i; g->a0[g->n] = a[0]; g->ret[g->n] = r; g->n++;
}

static void *arm(uint8_t *code, int slot, void *orig)
{
    size_t len;
    uint8_t *p = code + (size_t)slot * PT_TRAMPOLINE_MAX;
    assert(trampoline_emit((uint64_t)(uintptr_t)&ring, (uint64_t)(uintptr_t)orig,
                           (uint32_t)slot, p, PT_TRAMPOLINE_MAX, &len) == 0);
    return p;
}

int main(void)
{
    uint8_t *code = mmap(0, 4 * PT_TRAMPOLINE_MAX, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(code != MAP_FAILED);

    long (*t_nine)(long, long, long, long, long, long, long, long, long) = arm(code, 0, (void *)nine);
    double (*t_vsum)(int, ...) = arm(code, 1, (void *)vsum);
    struct pair128 (*t_wide)(long) = arm(code, 2, (void *)wide);

    assert(t_nine(1, 2, 3, 4, 5, 6, 7, 8, 9) == nine(1, 2, 3, 4, 5, 6, 7, 8, 9));
    assert(t_vsum(3, 1.5, 2.25, 4.0) == 7.75);
    struct pair128 w = t_wide(0x1234);
    assert(w.lo == 0x1234 && w.hi == ~0x1234L);

    assert(ring.head == 6);
    struct got g = { 0 };
    assert(ring_decode(&ring, sink, &g, 0) == 3);
    assert(g.idx[0] == 0 && g.a0[0] == 1 && g.ret[0] == 285);
    assert(g.idx[1] == 1 && g.a0[1] == 3);
    assert(g.idx[2] == 2 && g.a0[2] == 0x1234 && g.ret[2] == 0x1234);

    for (int k = 0; k < 100; k++)                     /* wraps the ring */
        assert(t_nine(k, 0, 0, 0, 0, 0, 0, 0, 1) == k + 9);
    memset(&g, 0, sizeof(g));
    ring_decode(&ring, 0, 0, 0);
    assert(ring.tail == ring.head && ring.dropped == 100 - PT_RING_CAP_EVENTS / 2);

    puts("trampoline_exec_test: OK");
    return 0;
}
