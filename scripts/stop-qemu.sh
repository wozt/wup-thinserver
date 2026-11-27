#!/usr/bin/env bash
# stop-qemu.sh — graceful shutdown of the ThinServer QEMU VM.
# Sends system_powerdown over the QMP socket, waits, then SIGTERMs the
# tracked PID only if the guest does not power off.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RUNDIR="$ROOT/build/run"
PIDFILE="$RUNDIR/qemu.pid"; QMP="$RUNDIR/qemu-qmp.sock"; MON="$RUNDIR/qemu-monitor.sock"

[ -f "$PIDFILE" ] || { echo "no qemu pidfile ($PIDFILE) — is the VM running?"; exit 0; }
PID="$(cat "$PIDFILE")"
# only ever signal the process this repo started
kill -0 "$PID" 2>/dev/null || { rm -f "$PIDFILE"; echo "stale pidfile removed"; exit 0; }
grep -q "wup-thinserver-qemu" "/proc/$PID/cmdline" 2>/dev/null \
    || { echo "pid $PID is not our qemu — refusing" >&2; exit 1; }

python3 - "$QMP" <<'PY' 2>/dev/null || true
import json, socket, sys, time
s = socket.socket(socket.AF_UNIX); s.settimeout(3)
s.connect(sys.argv[1])
s.recv(4096)
s.sendall(b'{"execute":"qmp_capabilities"}\n')
time.sleep(0.2); s.recv(4096)
s.sendall(b'{"execute":"system_powerdown"}\n')
time.sleep(0.2)
PY

for i in $(seq 1 60); do
    kill -0 "$PID" 2>/dev/null || { echo "VM powered off"; rm -f "$PIDFILE"; exit 0; }
    sleep 1
done
echo "guest did not power down in 60s — sending SIGTERM to $PID"
kill -TERM "$PID" 2>/dev/null || true
sleep 3
kill -0 "$PID" 2>/dev/null && kill -KILL "$PID" 2>/dev/null || true
rm -f "$PIDFILE"
echo "VM stopped"
