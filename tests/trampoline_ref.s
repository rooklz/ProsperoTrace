# ProsperoTrace trampoline — reference implementation (ground truth bytes).
#
# This file is the authoritative encoding for the machine code the C emitter
# (agent/src/trampoline.c) produces. tests/check_trampoline.sh assembles this
# file, extracts .text, and asserts the emitter's output matches byte-for-byte.
#
# Contract (see agent/src/ring.h):
#   - each hook = one trampoline = this sequence
#   - head is reserved atomically (+2: entry and ret lane-groups); head stays
#     even, so a pair never straddles the end of the ring
#   - the ret tag is cleared on entry and written last, so the reader can tell
#     an in-flight call from a finished one
#   - the original sees its exact register state (rax included, for varargs)
#     and the first eight stack-passed arguments; rax:rdx are returned as is
#
# Fixed patch points (imm64 operands):
#   RING_VA  - address of the shared ring (target VA)
#   ORIG_VA  - original function entry
#   TAG_ENTRY / TAG_RET - lane0 tag words ((PT_TAG_ENTRY<<32)|hook_index)

.intel_syntax noprefix
.globl tramp_entry
.text

.equ RING_VA,   0x1122334455667788
.equ ORIG_VA,   0x8877665544332211
.equ TAG_ENTRY, 0x0000000100000007   # tag=1, hook_index=7
.equ TAG_RET,   0x0000000200000007   # tag=2, hook_index=7

tramp_entry:
        push    rbp
        mov     rbp, rsp
        sub     rsp, 0x90               # 0x50 locals + 0x40 outgoing stack args
        mov     [rbp-0x08], rdi
        mov     [rbp-0x10], rsi
        mov     [rbp-0x18], rdx
        mov     [rbp-0x20], rcx
        mov     [rbp-0x28], r8
        mov     [rbp-0x30], r9
        mov     [rbp-0x50], rax         # al = vector-arg count for varargs

        # ---- reserve an even slot pair
        movabs  rax, RING_VA
        mov     edx, 2
        lock xadd [rax], edx            # edx = old head
        and     edx, 0x3e
        shl     rdx, 6
        lea     rdx, [rax+rdx+16]       # &slot[old head]
        mov     [rbp-0x38], rdx
        mov     qword ptr [rdx+0x40], 0 # ret tag: in flight

        # ---- entry event; tag written last
        mov     rax, [rbp-0x08]
        mov     [rdx+0x08], rax
        mov     rax, [rbp-0x10]
        mov     [rdx+0x10], rax
        mov     rax, [rbp-0x18]
        mov     [rdx+0x18], rax
        mov     rax, [rbp-0x20]
        mov     [rdx+0x20], rax
        mov     rax, [rbp-0x28]
        mov     [rdx+0x28], rax
        mov     rax, [rbp-0x30]
        mov     [rdx+0x30], rax
        movabs  r11, TAG_ENTRY
        mov     [rdx], r11

        # ---- forward up to eight stack-passed arguments
        mov     rax, [rbp+0x10]
        mov     [rsp+0x00], rax
        mov     rax, [rbp+0x18]
        mov     [rsp+0x08], rax
        mov     rax, [rbp+0x20]
        mov     [rsp+0x10], rax
        mov     rax, [rbp+0x28]
        mov     [rsp+0x18], rax
        mov     rax, [rbp+0x30]
        mov     [rsp+0x20], rax
        mov     rax, [rbp+0x38]
        mov     [rsp+0x28], rax
        mov     rax, [rbp+0x40]
        mov     [rsp+0x30], rax
        mov     rax, [rbp+0x48]
        mov     [rsp+0x38], rax

        # ---- call the original with the caller's registers
        mov     rdx, [rbp-0x18]
        mov     rax, [rbp-0x50]
        movabs  r11, ORIG_VA
        call    r11
        mov     [rbp-0x48], rax
        mov     [rbp-0x40], rdx

        # ---- ret event: lane7 = rax, then publish the tag
        mov     r11, [rbp-0x38]
        mov     [r11+0x78], rax
        movabs  rax, TAG_RET
        mov     [r11+0x40], rax

        mov     rax, [rbp-0x48]
        mov     rdx, [rbp-0x40]
        leave
        ret
