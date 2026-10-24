#!/bin/sh
# deploy.sh <agent.elf> — send the agent to the console's elfldr.
#
# elfldr starts payloads without arguments or environment, so the agent
# reads /data/prosperotrace.conf (see the README); upload it first. Start
# the receiver before deploying: the agent connects out to it.
#
#   PS5_IP=192.168.10.192 ./deploy.sh build/prosperotrace.elf
set -eu

ELF="${1:?usage: deploy.sh <agent.elf>}"
PS5_IP="${PS5_IP:?set PS5_IP to the console address}"
PORT="${PT_ELF_PORT:-9021}"

[ -f "$ELF" ] || { echo "no such file: $ELF" >&2; exit 1; }

SIZE=$(wc -c < "$ELF" | tr -d ' ')
NAME=$(basename "$ELF")
echo ">> sending $NAME ($SIZE bytes) to $PS5_IP:$PORT"
nc -w 5 "$PS5_IP" "$PORT" < "$ELF"
echo ">> sent; progress is in ftp://$PS5_IP:2121/data/prosperotrace.log"
