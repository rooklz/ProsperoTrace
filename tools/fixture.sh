#!/bin/sh
# fixture.sh — trace the ptdemo fixture on a console and diff it against the
# stored baseline (tests/fixtures/ptdemo-baseline.jsonl).
#
#   PS5_IP=192.168.10.192 HOST_IP=192.168.10.190 \
#   ANALYTICS=../prospero-analytics tools/fixture.sh [--update]
#
# Needs: ps5-payload-sdk (PS5_PAYLOAD_SDK, or it is searched in common
# locations), the console's elfldr on 9021 and its FTP server on 2121 (for
# /data/prosperotrace.conf), and a checkout of rooklz/prospero-analytics
# for the receiver and tracediff. An LLVM toolchain (Homebrew llvm is
# found automatically) is needed to build the agent and ptdemo.
#
# Each capture is checked against prospero-analytics' EVENT_FORMAT.md
# (host/validate.py). With FIXTURE_OUT set, the capture, a tracedb database
# and the dashboard are left there.
#
# Exit 0 when the captured API surface matches the baseline (same functions;
# rate changes are reported, not fatal), 1 when the format is broken or
# functions surfaced or went away, 2 when the run itself failed. --update
# replaces the baseline.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PS5_IP="${PS5_IP:?set PS5_IP to the console address}"
HOST_IP="${HOST_IP:?set HOST_IP to the address the console uses to reach this host}"
ANALYTICS="${ANALYTICS:?set ANALYTICS to a prospero-analytics checkout}"
SDK="${PS5_PAYLOAD_SDK:-}"

# Locate the payload SDK if the caller did not export PS5_PAYLOAD_SDK.
if [ -z "$SDK" ]; then
    for d in "$HOME/ps5/dev/ps5-payload-sdk" /opt/ps5-payload-sdk; do
        [ -f "$d/toolchain/prospero.cmake" ] && SDK="$d" && break
    done
fi
if [ -z "$SDK" ] || [ ! -f "$SDK/toolchain/prospero.cmake" ]; then
    echo "fixture: set PS5_PAYLOAD_SDK to a ps5-payload-sdk checkout" >&2
    exit 2
fi

# The SDK's tool wrappers need an LLVM toolchain discoverable via
# llvm-config. Prefer the caller's choice, else look for Homebrew's.
if ! "$SDK/bin/prospero-llvm-config" --bindir >/dev/null 2>&1; then
    for cfg in /opt/homebrew/opt/llvm/bin/llvm-config /usr/local/opt/llvm/bin/llvm-config; do
        if [ -x "$cfg" ]; then
            export LLVM_CONFIG="$cfg"
            break
        fi
    done
fi
if [ -n "${LLVM_CONFIG:-}" ]; then
    BD=$("$SDK/bin/prospero-llvm-config" --bindir)
    case ":$PATH:" in
        *":$BD:"*) ;;
        *) export PATH="$BD:$PATH" ;;
    esac
fi
FTP="ftp://$PS5_IP:${PS5_FTP_PORT:-2121}"
BASELINE="$ROOT/tests/fixtures/ptdemo-baseline.jsonl"
UPDATE=0
[ "${1:-}" = "--update" ] && UPDATE=1

WORK=$(mktemp -d)
RX= ECHO=
cleanup() {
    for p in $RX $ECHO; do
        kill "$p" 2>/dev/null || true
        wait "$p" 2>/dev/null || true
    done
    rm -rf "$WORK"
}
trap cleanup EXIT
die() { echo "fixture: $*" >&2; exit 2; }

echo ">> building agent and ptdemo"
cmake -S "$ROOT/agent" -B "$WORK/build" -DCMAKE_TOOLCHAIN_FILE="$SDK/toolchain/prospero.cmake" > "$WORK/cmake.log" 2>&1 \
    || { cat "$WORK/cmake.log"; die "cmake configure failed"; }
cmake --build "$WORK/build" > "$WORK/build.log" 2>&1 || { cat "$WORK/build.log"; die "agent build failed"; }
"$SDK/bin/prospero-clang" -O2 -Wall -DPTDEMO_HOST="\"$HOST_IP\"" "$ROOT/demo/ptdemo.c" -lpthread \
    -o "$WORK/ptdemo.elf" || die "ptdemo build failed"

printf 'PT_HOST=%s\nPT_PORT=9029\nPT_PID_FILE=/data/ptdemo.pid\nPT_WAIT=20\n' "$HOST_IP" > "$WORK/prosperotrace.conf"
curl -sf --max-time 10 -T "$WORK/prosperotrace.conf" "$FTP/data/prosperotrace.conf" || die "cannot upload config over FTP"

echo ">> starting receiver (9029) and echo server (9030)"
python3 "$ANALYTICS/host/receiver.py" --port 9029 --out "$WORK/traces" -q > "$WORK/recv.log" 2>&1 &
RX=$!
python3 -c '
import socketserver
class Echo(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.sendall(self.request.recv(64))
socketserver.ThreadingTCPServer.allow_reuse_address = True
socketserver.ThreadingTCPServer(("0.0.0.0", 9030), Echo).serve_forever()' > /dev/null 2>&1 &
ECHO=$!
sleep 1

curl -s --max-time 5 -Q "DELE /data/prosperotrace.log" "$FTP/" -o /dev/null 2>&1 || true  # a stale log would look done
echo ">> sending ptdemo, then the agent, to $PS5_IP:9021"
nc -w 5 "$PS5_IP" 9021 < "$WORK/ptdemo.elf" || die "elfldr not reachable"
sleep 2
nc -w 5 "$PS5_IP" 9021 < "$WORK/build/prosperotrace.elf" || die "elfldr not reachable"

# ptdemo waits 8 s, runs 5 s, lingers 1 s; the agent logs "done" once it
# has drained the ring and detached
for _ in $(seq 1 60); do
    sleep 1
    curl -sf --max-time 5 "$FTP/data/prosperotrace.log" 2>/dev/null | grep -q "prosperotrace: done" && break
done
kill -INT "$RX"; wait "$RX" 2>/dev/null || true; RX=
curl -sf --max-time 5 "$FTP/data/prosperotrace.log" > "$WORK/agent.log" || true
sed 's/^/   agent: /' "$WORK/agent.log" | grep -v "hooked" || true

CAPTURE=$(ls -t "$WORK"/traces/trace-*.jsonl 2>/dev/null | head -1)
[ -n "$CAPTURE" ] || die "no capture received (see agent log above)"
CALLS=$(grep -c '"type":"call"' "$CAPTURE" || true)
[ "$CALLS" -gt 0 ] || die "capture has no call events (see agent log above)"
echo ">> captured $CALLS calls"

# the agent/host contract: a format change on either side fails here
python3 "$ANALYTICS/host/validate.py" "$CAPTURE" || { echo "fixture: capture breaks EVENT_FORMAT.md"; exit 1; }

OUT="${FIXTURE_OUT:-}"
if [ -n "$OUT" ]; then
    cp "$CAPTURE" "$OUT/"
    python3 "$ANALYTICS/host/tracedb.py" import "$OUT/fixture.db" "$CAPTURE" > /dev/null
    python3 "$ANALYTICS/host/dashboard.py" "$OUT/fixture.db" -o "$OUT/fixture.html" > /dev/null
    echo ">> capture, database and dashboard in $OUT"
fi

if [ "$UPDATE" = 1 ] || [ ! -f "$BASELINE" ]; then
    mkdir -p "$(dirname "$BASELINE")"
    cp "$CAPTURE" "$BASELINE"
    echo ">> baseline written: $BASELINE"
    exit 0
fi

python3 "$ANALYTICS/host/tracediff.py" "$BASELINE" "$CAPTURE" --normalize --json "$WORK/diff.json" \
    | grep -v '^\[json\]'
python3 - "$WORK/diff.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
moved = [f"+{e['mod']}.{e['fn']}" for e in d["surfaced"]] + [f"-{e['mod']}.{e['fn']}" for e in d["gone"]]
if moved:
    print("fixture: API surface changed: " + " ".join(moved))
    sys.exit(1)
print("fixture: API surface matches the baseline")
EOF
