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
target ──GOT──▶ per-fn trampoline ──▶ shared stub
                                        │  (capture args, call original
                                        │   via saved slot, capture ret)
                                        ▼
                              SPSC ring buffer (64 KiB)
                                        │  consumer thread (agent process)
                                        ▼
                              batched TCP ──▶ host receiver (JSONL)
```

Backpressure policy: never block the target on I/O. If the ring is full the
stub increments a drop counter; the consumer emits a `{"type":"drops"}` record.
Losing events is always preferable to stalling the traced process.

The stub runs with interrupts of the target thread intact and preserves all
argument registers and the red zone; only the trampoline's own scratch
registers are clobbered (see `agent/src/hook.c` for the exact contract).

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
2. **M2** — module enumeration in target + PLT/GOT hooking (current)
3. **M3** — filter language (module/function globs), detach/reattach
4. **M4** — inline-detour fallback; option to follow child processes
