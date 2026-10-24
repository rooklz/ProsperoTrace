# ProsperoTrace trampoline — reference implementation (ground truth bytes).
#
# This file is the authoritative encoding for the machine code the C emitter
# (agent/src/trampoline.c) produces. tests/check_trampoline.sh assembles this
# file, extracts .text, and asserts the emitter's output matches byte-for-byte.
#
# Contract (see agent/src/ring.h):
#   - each hook = one trampoline = this sequence
#   - entry block writes an 8-lane event, rounds `head` by +2 (ret lane reserved)
#   - calls the original, then writes the ret lane
#   - never clobbers callee-saved registers; frame 0x50 bytes; RWX-not-needed
#
# Fixed patch points (imm64 operands):
#   RING_VA  - address of the shared ring (target VA)
#   ORIG_VA  - original function entry; used for the call and the tail jump
#   TAG_ENTRY / TAG_RET - lane0 tag words ((PT_TAG_ENTRY<<32)|hook_index)
#
# Not position-independent by design: every template is placed and patched
# once per hook with absolute addresses. See trampoline.c for the byte table.

.intel_syntax noprefix
.globl tramp_entry
.text

.equ RING_VA,   0x1122334455667788
.equ ORIG_VA,   0x8877665544332211
.equ TAG_ENTRY, 0x0000000100000007   # tag=1, hook_index=7
.equ TAG_RET,   0x0000000200000007   # tag=2, hook_index=7

tramp_entry:
        # ---- prologue: preserve all callee-saved, save six arg regs
        push    rbp
        mov     rbp, rsp
        sub     rsp, 0x50
        mov     [rbp-0x08], rdi
        mov     [rbp-0x10], rsi
        mov     [rbp-0x18], rdx
        mov     [rbp-0x20], rcx
        mov     [rbp-0x28], r8
        mov     [rbp-0x30], r9

        # ---- entry event
        movabs  rax, RING_VA
        mov     edx, [rax]              # head
        and     edx, 0x3f               # % PT_RING_CAP_EVENTS
        shl     rdx, 6                  # * PT_EVENT_LANES(8) * 8
        lea     rdx, [rax+rdx+16]       # &slots[idx]
        movabs  r11, TAG_ENTRY
        mov     [rdx], r11              # lane0 = tag word
        mov     rax, [rbp-0x08]         # lane1..6 = args
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
        mov     [rbp-0x40], rax        # stash ring ptr
        mov     [rbp-0x38], rdx        # stash entry slot ptr
        add     dword ptr [rax], 2      # reserve entry+ret lanes

        # ---- call original; rax = return value
        movabs  rax, ORIG_VA
        call    rax
        mov     [rbp-0x48], rax

        # ---- ret event (slot = entry_slot + 8 lanes)
        mov     rdx, [rbp-0x38]
        add     rdx, 0x40
        movabs  r11, TAG_RET
        mov     [rdx], r11
        mov     rax, [rbp-0x48]
        mov     [rdx+0x38], rax         # lane7 = ret value

        # ---- epilogue: restore frame and tail-jump original
        add     rsp, 0x50
        pop     rbp
        movabs  rax, ORIG_VA
        jmp     rax