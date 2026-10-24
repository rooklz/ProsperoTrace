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

Working end to end on a console (fw 13.40): the agent attaches to a running
payload, hooks its imports, streams every call, and detaches cleanly. The
console fixture (`tools/fixture.sh`) traces `demo/ptdemo.c` (TCP echo client,
file I/O, heap churn, a worker thread) and diffs it against the stored
baseline: ~650 calls over 21 functions in libkernel and libSceLibcInternal,
0 dropped.

- [x] host receiver, summarizer, diff, SQLite query layer, dashboard
      ([prospero-analytics](https://github.com/rooklz/prospero-analytics))
- [x] NID table generator (`tools/nidgen.py`) — text list → C table
- [x] agent: connect-out streaming with SPSC ring buffer + drop accounting
- [x] agent: ptrace attach with debugger credentials, signal forwarding,
      stop-restore-detach on exit
- [x] agent: remote `mmap` in the target (one single-stepped syscall), code
      pages flipped to RX with `kernel_mprotect`
- [x] agent: GOT hooking of `JUMP_SLOT` and `GLOB_DAT` imports (NID-named
      and plain-named), module attribution from `DT_NEEDED`
- [x] agent: trampoline with atomic slot reservation, full register and
      stack-argument forwarding (executed in `tests/trampoline_exec_test.c`)
- [x] agent: remote ring reader that keeps blocking calls and counts laps
- [x] config file (`/data/prosperotrace.conf`) and log
      (`/data/prosperotrace.log`), since elfldr passes no environment
- [x] console fixture (`tools/fixture.sh`, `.github/workflows/fixture.yml`)

Known limits: the tracer hooks the main executable's imports (calls between
system libraries are not seen); `tid` is 0; calls in the last ~2 ms before
the target exits can be lost, since the ring lives in the target's memory;
functions that return twice or never return (`setjmp`, `longjmp`, `exit`,
...) are never hooked.

## Layout

```
agent/    on-console tracer (C, ps5-payload-sdk) — streams events out over TCP
tools/    build-time generators (NID table)
docs/     design notes
demo/     ptdemo, the fixture workload
tests/    host-run checks (emitter bytes and execution, decoder, filter,
          escaping, config) and the console fixture baseline
tools/fixture.sh  trace ptdemo on a console and diff against the baseline
deploy.sh pushes agent.elf to $PS5_IP:9021 (elfldr)
```

The host-side capture and analysis tools (receiver, summarizer,
API-surface diff) live in the separate
**[rooklz/prospero-analytics](https://github.com/rooklz/prospero-analytics)**
repo, which consumes only the JSONL event stream
(`docs/EVENT_FORMAT.md` there).

## Quickstart (host side)

```sh
git clone https://github.com/rooklz/prospero-analytics
cd prospero-analytics
python3 host/receiver.py --port 9029 --out traces/
python3 host/summarize.py traces/trace-1234-*.jsonl
python3 host/tracediff.py traces/before.jsonl traces/after.jsonl
```

## Quickstart (agent)

```sh
cd agent
cmake -B build -DCMAKE_TOOLCHAIN_FILE=$PS5_PAYLOAD_SDK/toolchain/prospero.cmake
cmake --build build

# elfldr gives payloads no environment: configure through /data
printf 'PT_HOST=192.168.10.190\nPT_PID=1234\n' > prosperotrace.conf
curl -T prosperotrace.conf ftp://192.168.10.192:2121/data/prosperotrace.conf
PS5_IP=192.168.10.192 ../deploy.sh build/prosperotrace.elf
curl ftp://192.168.10.192:2121/data/prosperotrace.log     # agent progress
```

Keys (see `agent/src/main.c`): `PT_HOST`, `PT_PORT`, `PT_PID` / `PT_NAME` /
`PT_PID_FILE`, `PT_BASE` + `PT_DYNAMIC` (elfldr payloads, whose ELF headers
are not mapped), `PT_FILTER` (comma-separated `mod.fn` globs). The agent
connects out to the host, so the receiver must be listening first.

## Console fixture

```sh
PS5_IP=192.168.10.192 HOST_IP=192.168.10.190 ANALYTICS=../prospero-analytics \
PS5_PAYLOAD_SDK=~/ps5-payload-sdk tools/fixture.sh            # diff vs baseline
tools/fixture.sh --update                                      # re-record it
```

Exits 1 if a function surfaced or disappeared compared to
`tests/fixtures/ptdemo-baseline.jsonl`; rate changes are printed per 1000
calls. Every capture is also validated against prospero-analytics'
`docs/EVENT_FORMAT.md`, so a format change on either side fails the
fixture. `.github/workflows/fixture.yml` runs it on the self-hosted `ps5`
runner and publishes the capture, its `tracedb` database and the dashboard
as the `fixture` artifact. For tracing anything else, see the end-to-end
guide: [prospero-analytics/docs/GUIDE.md](https://github.com/rooklz/prospero-analytics/blob/main/docs/GUIDE.md).

## Event format

One JSON object per line:

```json
{"type":"hello","pid":1234,"title":"ftpsrv","fw":"13.40","seq":0}
{"type":"call","seq":3,"ts":20049976182,"tid":0,"mod":"libkernel","fn":"socket","nid":"0x0","args":["0x2","0x1","0x0","0x0","0x0","0x0"],"ret":"0x7"}
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
