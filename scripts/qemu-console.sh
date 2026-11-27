#!/usr/bin/env bash
# qemu-console.sh — attach to the VM serial console.
# Falls back to printing the serial log if no console client is attached.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SOCK="$ROOT/build/run/serial.sock"
if command -v socat >/dev/null; then
    exec socat - "UNIX-CONNECT:$SOCK"
elif command -v nc >/dev/null && nc -h 2>&1 | grep -q -- '-U'; then
    exec nc -U "$SOCK"
else
    echo "no socat/nc-unix: tailing $ROOT/build/run/serial.log instead" >&2
    exec tail -f "$ROOT/build/run/serial.log"
fi
