# ProsperoTrace design notes

## Goal

Observe the OS-facing call surface of a running PS5 userland process from
outside that process, on retail-ish hardware, without modifying the target
on disk. Produce a stream of call events a host PC can record and analyze.

## Process access model

The agent runs as its own payload process (loaded via elfldr, port 9021) and
inspects the target through the kernel's debugging interface
(`ptrace(PT_ATTACH)` + `PT_READ_D` / `PT_WRITE_D` for memory). This is the
same access model `ps5debug` and WebRTE use, so the required kernel state is
already established by the standard homebrew enabler stack; no custom kernel
code is needed.

Why not in-process injection (LD_PRELOAD-style)? There is no dynamic loader
control over already-running retail processes, and injecting a library into a
running process requires the same ptrace machinery plus remote calling. ptrace
keeps v1 small and auditable. An in-process variant (as an etaHEN plugin) is a
possible v2 for lower overhead.

## Where to intercept

Two candidate sites were considered:

### PLT/GOT patching (chosen for v1)

Calls into shared modules (libkernel, libSce*) resolve through the target's
procedure linkage table. Overwriting resolved GOT slots with a trampoline
captures exactly the cross-module calls we care about, at process scope,
with the real call site preserved (return address on the stack).

Cost model: we parse the target's `.dynamic`/`.rela.plt` remotely (one read
of the ELF header + program headers + dynamic section), match `R_X86_64_JUMP_SLOT`
relocations against our NID filter set, and swap matching GOT entries for a
trampoline address. Trampolines live in a page the tracer allocates in the
target (`PT_SYSCTRL`/`ptrace` mmap) — one per hooked function, plus a shared
stub that gathers (rdi, rsi, rdx, rcx, r8, r9) and the return address.

Pros: exact module+function identity from the relocation; tiny hot-path cost;
no disassembly of library code needed. Cons: misses direct `syscall`
instructions and intra-module calls (acceptable — the syscall wrapper in
libkernel is itself an exported function we hook).

### Inline detours on library exports (fallback, v1.1)

Patch the first bytes of each exported function with a jump to a stub.
Catches intra-library callers too, but requires instruction-length-aware
relocated prologues and patching shared (potentially busy) code pages in
every process that maps the library. More moving parts; deferred.

## Event pipeline

```
target ──GOT──▶ trampoline ──stores──▶ shared ring (in-target page)
                                              │  reader thread (agent)
                                              ▼
                                       pt_log_call → agent SPSC ring
                                              │  consumer thread
                                              ▼
                                 batched TCP ──▶ host receiver (JSONL)
```

Two buffers by design:

- **Shared ring** (target→agent). The trampolines write fixed-size events
  (8×u64 lanes per event; entry and ret lane-groups) with plain stores, and
  the agent reader drains from the same page. No locks — the target bumps
  `head` per event pair and the reader never races past it. Layout in
  `agent/src/ring.h`.
- **Stream ring** (agent→socket). The existing SPSC ring in `stream.c`
  batches events into TCP sends; this is where drops are counted and
  reported as `{"type":"drops"}` records.

Backpressure policy: never block the target on I/O. If the shared ring is
full the trampoline skips the write — insertion is a single non-atomic
`head` bump, so under heavy concurrency the tracer may lose a race rather
than stall; per-thread slot reservation (seqlock-style) is M4.

The trampolines are emitted by `agent/src/trampoline.c` from the canonical
byte sequence in `tests/trampoline_ref.s`; `tests/check_trampoline.sh`
asserts the two never drift.

## Identity of functions

Imports are named by NID (the platform's 64-bit symbol hash). The agent
carries a generated table (`agent/data/nids_table.c`, produced by
`tools/nidgen.py` from a text list) mapping NID → (module, name). Unknown
NIDs are logged as `nid_0x…` so coverage gaps are visible in the output.

## Failure rules

- Every pointer into target memory is validated (mapping check) before read
  and re-validated before write.
- Any ptrace error during hook installation restores every slot touched so
  far and detaches.
- Target exit is detected via `PT_EVENT`; the agent flushes the ring, closes
  the stream and exits 0.

## Roadmap

1. **M1** — streamer, ring buffer, process discovery, host receiver ✅
2. **M2** — PLT/GOT hooks: trampoline emitter (byte-verified), shared-ring
   transport with reader thread, candidate scan ✅ — remaining:
   `PT_SYSCTRL` remote map glue and the GOT write-up on console
3. **M3** — filter language (module/function globs), detach/reattach
4. **M4** — inline-detour fallback; per-thread pending events (seqlock); option
   to follow child processes
