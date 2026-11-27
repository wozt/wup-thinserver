#!/usr/bin/env bash
# qemu-smoke.sh — quick health check for the ARM64 ThinServer VM,
# whether launched by scripts/run-qemu.sh or libvirt.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HPORT="${WUP_QEMU_HOST_PORT:-5080}"
SPORT="${WUP_QEMU_SSH_PORT:-2222}"
rc=0

echo "[smoke] UDP $HPORT -> guest:50001 (WUP beacon probe)"
python3 - "$HPORT" <<'PY'
import socket, sys
port = int(sys.argv[1])
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", 0)); s.settimeout(4)
for _ in range(3):
    s.sendto(b"B", ("127.0.0.1", port))
    try:
        data, addr = s.recvfrom(256)
        print(f"[smoke]   reply {data[:1]!r} from {addr}")
        sys.exit(0)
    except socket.timeout:
        pass
print("[smoke]   NO REPLY — daemon not streaming yet or VM down")
sys.exit(1)
PY
[ $? -ne 0 ] && rc=1

if [ "${WUP_QEMU_SSH_PORT:-2222}" != "0" ]; then
    echo "[smoke] SSH admin@127.0.0.1:$SPORT"
    if ssh -i "$ROOT/build/images/qemu-admin-key" -p "$SPORT" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=8 -o BatchMode=yes admin@127.0.0.1 \
        'echo "[smoke]   guest: $(uname -m), $(systemctl is-active wup-thinserver)"' 2>/dev/null; then
        :
    else
        echo "[smoke]   SSH FAILED"; rc=1
    fi
fi
exit $rc
