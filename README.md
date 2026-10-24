# ProsperoTrace

`strace`/`ltrace`-style call tracing for PS5 userland processes.

ProsperoTrace records which OS-library functions a running process calls —
with timestamps, arguments and return values — and streams the events to a
host PC as newline-delimited JSON. It exists to give homebrew developers and
compatibility-layer authors ground truth about software behavior on real
hardware, instead of guessing from documentation.

This is a development/instrumentation tool. It ships no Sony code, headers or
binaries; it builds exclusively on the public homebrew SDKs
([ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk),
[etaHEN-SDK](https://github.com/LightningMods/etaHEN-SDK)).

## Status

Early development. What works today:

- [x] host receiver (`host/receiver.py`) — full JSONL capture, live filters, summary
- [x] trace summarizer (`host/summarize.py`)
- [x] NID table generator (`tools/nidgen.py`) — text list → C header
- [x] agent: connect-out streaming with SPSC ring buffer + drop accounting
- [x] agent: process discovery (`kern.proc` sysctl)
- [x] agent: trampoline emitter (x86-64, byte-verified against a reference — `tests/check_trampoline.sh`)
- [x] agent: shared ring transport + decoder with entry/ret pairing (host-tested — `tests/ring_decode_test.c`)
- [x] agent: hook selection filter (`PT_FILTER`, globs on `mod.fn`; host-tested — `tests/filter_test.c`)
- [x] agent: PLT/GOT scan + candidate collection
- [ ] agent: remote page injection glue (`PT_SYSCTRL` map) + GOT write-up — the last step before on-console arming
- [ ] agent: dynlib module walk (instrument loaded PRXs beyond the main executable)
- [ ] agent: inline-detour fallback for intra-module calls
- [ ] NID table coverage beyond the seed set
- [ ] `deploy.sh` end-to-end run against a real title

## Layout

```
agent/    on-console tracer (C, ps5-payload-sdk) — streams events out over TCP
host/     receiver + summarizer (Python 3, stdlib only)
tools/    build-time generators (NID table)
docs/     design notes
deploy.sh pushes agent.elf to $PS5_IP:9021 (elfldr)
```

## Quickstart (host side)

```sh
python3 host/receiver.py --port 9029 --out traces/
python3 host/summarize.py traces/trace-1234-*.jsonl
```

## Quickstart (agent)

```sh
cd agent
cmake -B build -DCMAKE_TOOLCHAIN_FILE=$PS5_PAYLOAD_SDK/cmake/toolchain-ps5.cmake
cmake --build build
PS5_IP=192.168.10.192 ./deploy.sh build/prosperotrace.elf --pid 1234
```

The agent connects out to the host (`--host` / `PT_HOST` env), so the
receiver must be listening first.

## Event format

One JSON object per line:

```json
{"type":"hello","pid":1234,"title":"ftpsrv","fw":"13.40","seq":0}
{"type":"call","seq":1,"ts":18273645,"tid":100051,"mod":"libkernel","fn":"sceKernelOpen","args":["0x2000a340","0x0","0x0"],"ret":"0x5"}
{"type":"drops","seq":2,"n":137}
```

Timestamps are microseconds from the agent's monotonic clock. Pointer and
integer arguments are recorded as hex strings; pointer chasing is the
caller's job (the agent never dereferences target memory for logging).

## Why

Compatibility layers (e.g. [AnyPS5](https://github.com/boykopovar/AnyPS5))
re-implement hundreds of OS-facing functions. The fastest way to know *which*
functions matter and *how* they're actually used — argument patterns, call
order, threading behavior — is to watch real software call them on a real
console. Public documentation tells you what a function should do; a trace
tells you what the software actually needs.

## License

MIT. Clean-room: no Sony material is distributed.
