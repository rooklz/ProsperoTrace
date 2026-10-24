#include "trampoline.h"
#include "ring.h"

#include <stdint.h>
#include <string.h>

/*
 * Byte template for the trampoline. The immediates below are unique
 * placeholders, one per patch site, so patching is a simple scan-and-replace
 * with no hardcoded offsets. The canonical bytes live in
 * tests/trampoline_ref.s; tests/check_trampoline.sh asserts they match.
 *
 * Placeholders (little-endian on disk):
 *   0x11...  ring_va       (movabs rax)
 *   0x22...  entry tag     (movabs r11)
 *   0x33...  orig (call)   (movabs rax)
 *   0x44...  ret tag       (movabs r11)
 *   0x55...  orig (jmp)    (movabs rax)
 */
static const uint8_t k_tramp[] = {
    0x55,                                            /* push rbp */
    0x48, 0x89, 0xe5,                                /* mov rbp, rsp */
    0x48, 0x83, 0xec, 0x50,                          /* sub rsp, 0x50 */
    0x48, 0x89, 0x7d, 0xf8,                          /* mov [rbp-0x08], rdi */
    0x48, 0x89, 0x75, 0xf0,                          /* mov [rbp-0x10], rsi */
    0x48, 0x89, 0x55, 0xe8,                          /* mov [rbp-0x18], rdx */
    0x48, 0x89, 0x4d, 0xe0,                          /* mov [rbp-0x20], rcx */
    0x4c, 0x89, 0x45, 0xd8,                          /* mov [rbp-0x28], r8 */
    0x4c, 0x89, 0x4d, 0xd0,                          /* mov [rbp-0x30], r9 */
    0x48, 0xb8, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, /* movabs rax, ring_va */
    0x8b, 0x10,                                      /* mov edx, [rax] */
    0x83, 0xe2, 0x3f,                                /* and edx, 0x3f */
    0x48, 0xc1, 0xe2, 0x06,                          /* shl rdx, 6 */
    0x48, 0x8d, 0x54, 0x10, 0x10,                    /* lea rdx, [rax+rdx+16] */
    0x49, 0xbb, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, /* movabs r11, tag */
    0x4c, 0x89, 0x1a,                                /* mov [rdx], r11 */
    0x48, 0x8b, 0x45, 0xf8, 0x48, 0x89, 0x42, 0x08,  /* arg0 -> lane1 */
    0x48, 0x8b, 0x45, 0xf0, 0x48, 0x89, 0x42, 0x10,  /* arg1 -> lane2 */
    0x48, 0x8b, 0x45, 0xe8, 0x48, 0x89, 0x42, 0x18,  /* arg2 -> lane3 */
    0x48, 0x8b, 0x45, 0xe0, 0x48, 0x89, 0x42, 0x20,  /* arg3 -> lane4 */
    0x48, 0x8b, 0x45, 0xd8, 0x48, 0x89, 0x42, 0x28,  /* arg4 -> lane5 */
    0x48, 0x8b, 0x45, 0xd0, 0x48, 0x89, 0x42, 0x30,  /* arg5 -> lane6 */
    0x48, 0x89, 0x45, 0xc0,                          /* mov [rbp-0x40], rax (ring) */
    0x48, 0x89, 0x55, 0xc8,                          /* mov [rbp-0x38], rdx (slot) */
    0x83, 0x00, 0x02,                                /* add dword [rax], 2 */
    0x48, 0xb8, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, /* movabs rax, orig */
    0xff, 0xd0,                                      /* call rax */
    0x48, 0x89, 0x45, 0xb8,                          /* mov [rbp-0x48], rax (ret) */
    0x48, 0x8b, 0x55, 0xc8,                          /* mov rdx, [rbp-0x38] */
    0x48, 0x83, 0xc2, 0x40,                          /* add rdx, 0x40 (8 lanes) */
    0x49, 0xbb, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, /* movabs r11, ret_tag */
    0x4c, 0x89, 0x1a,                                /* mov [rdx], r11 */
    0x48, 0x8b, 0x45, 0xb8, 0x48, 0x89, 0x42, 0x38,  /* ret -> lane7 */
    0x48, 0x83, 0xc4, 0x50,                          /* add rsp, 0x50 */
    0x5d,                                            /* pop rbp */
    0x48, 0xb8, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, /* movabs rax, orig */
    0xff, 0xe0,                                      /* jmp rax */
};

#define TP_PH_RING   0x1111111111111111ull
#define TP_PH_TAG_E  0x2222222222222222ull
#define TP_PH_ORIG_C 0x3333333333333333ull
#define TP_PH_TAG_R  0x4444444444444444ull
#define TP_PH_ORIG_J 0x5555555555555555ull

static int patch_imm(uint8_t *p, size_t n, uint8_t prefix, uint8_t opcode, uint64_t placeholder, uint64_t value)
{
    uint8_t pat[10];
    pat[0] = prefix;
    pat[1] = opcode;                   /* b8 = movabs r64, imm64 (r11 = bb) */
    memcpy(pat + 2, &placeholder, 8);

    size_t hits = 0;
    for (size_t i = 0; i + sizeof(pat) <= n; i++) {
        if (memcmp(p + i, pat, sizeof(pat)) == 0) {
            memcpy(p + i + 2, &value, 8);
            hits++;
        }
    }
    return hits == 1 ? 0 : -1;
}

int trampoline_emit(uint64_t ring_va, uint64_t orig_va, uint32_t hook_index,
                    uint8_t *out, size_t cap, size_t *out_len)
{
    if (cap < sizeof(k_tramp))
        return -1;

    memcpy(out, k_tramp, sizeof(k_tramp));

    if (patch_imm(out, sizeof(k_tramp), 0x48, 0xb8, TP_PH_RING, ring_va) != 0)
        return -1;
    if (patch_imm(out, sizeof(k_tramp), 0x49, 0xbb, TP_PH_TAG_E, pt_make_tag(PT_TAG_ENTRY, hook_index)) != 0)
        return -1;
    if (patch_imm(out, sizeof(k_tramp), 0x48, 0xb8, TP_PH_ORIG_C, orig_va) != 0)
        return -1;
    if (patch_imm(out, sizeof(k_tramp), 0x49, 0xbb, TP_PH_TAG_R, pt_make_tag(PT_TAG_RET, hook_index)) != 0)
        return -1;
    if (patch_imm(out, sizeof(k_tramp), 0x48, 0xb8, TP_PH_ORIG_J, orig_va) != 0)
        return -1;

    if (out_len)
        *out_len = sizeof(k_tramp);
    return 0;
}