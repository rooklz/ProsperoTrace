#!/bin/sh
# deploy.sh <agent.elf> [--pid N] [--host IP] [--port N]
# Sends the agent ELF to the console's elfldr, then prints how to launch.
set -eu

ELF="${1:?usage: deploy.sh <agent.elf> [--pid N] [--host IP] [--port N]}"
shift || true

PS5_IP="${PS5_IP:?set PS5_IP to the console's address}"
PORT="${PT_ELF_PORT:-9021}"

[ -f "$ELF" ] || { echo "no such file: $ELF" >&2; exit 1; }

echo ">> sending $(basename "$ELF") ($(wc -c < "$ELF" | tr -d ' ') bytes) to $PS5_IP:$PORT"
nc -N "$PS5_IP" "$PORT" < "$ELF"
echo ">> sent. Start the receiver first (see github.com/rooklz/prospero-analytics): python3 host/receiver.py --port 9029"
echo ">> agent args are passed via env on the console side (see agent/src/main.c)"
