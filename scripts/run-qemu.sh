#!/usr/bin/env bash
# run-qemu.sh — launch the ARM64 ThinServer VM (headless).
#
# Profiles:
#   (default)   functional: 6 vCPU, 8 GiB, virt machine, software x264
#   --profile pi4      Raspberry Pi 4 approximation: cortex-a72, 4 vCPU, 2 GiB
#   --profile zero2w   Raspberry Pi Zero 2 W approximation: cortex-a53,
#                      4 vCPU, 512 MiB (reduced footprint)
#
# Networking: QEMU user-mode (slirp) with a UDP hostfwd rule —
#   host UDP <host_port>  ->  guest UDP 50001
# The stabilized server endpoint means replies leave from guest:50001 and
# slirp returns them through the same host port the client dialed.
#
# Env overrides: WUP_QEMU_HOST_ADDR (default 0.0.0.0 = LAN-reachable;
# set 127.0.0.1 to restrict), WUP_QEMU_HOST_PORT (default 5080),
# WUP_QEMU_GUEST_PORT (default 50001), WUP_QEMU_MEM/CPUS,
# WUP_QEMU_SSH_PORT (default 2222; set 0 to disable the admin SSH forward).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="$ROOT/build/images/wup-thinserver-qemu-arm64.qcow2"
SEED="$ROOT/build/images/seed.img"
RUNDIR="$ROOT/build/run"; mkdir -p "$RUNDIR"
PIDFILE="$RUNDIR/qemu.pid"; MON="$RUNDIR/qemu-monitor.sock"
SERIAL="$RUNDIR/serial.log"; QMP="$RUNDIR/qemu-qmp.sock"

PROFILE=func; while [ $# -gt 0 ]; do case "$1" in
    --profile) PROFILE="$2"; shift 2;;
    -h|--help) sed -n '2,25p' "$0"; exit 0;;
    *) echo "unknown arg $1" >&2; exit 2;;
esac; done

# duplicate-launch detection
if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    echo "qemu already running (pid $(cat "$PIDFILE"))" >&2; exit 1
fi
# never grab the disk while the libvirt-managed copy is running
if command -v virsh >/dev/null 2>&1 && \
   virsh -c qemu:///system domstate wup-thinserver-arm64 2>/dev/null | grep -q running; then
    echo "libvirt domain wup-thinserver-arm64 is running — use Virt-Manager/virsh, or ./scripts/register-libvirt.sh --stop first" >&2
    exit 1
fi
[ -f "$IMG" ] || { echo "missing image $IMG — run scripts/build-qemu-image.sh" >&2; exit 1; }
[ -f "$SEED" ] || { echo "missing seed $SEED — run scripts/build-qemu-image.sh" >&2; exit 1; }

case "$PROFILE" in
    func)   CPU=cortex-a72; MEM="${WUP_QEMU_MEM:-8G}"; CPUS="${WUP_QEMU_CPUS:-6}";;
    pi4)    CPU=cortex-a72; MEM="${WUP_QEMU_MEM:-2G}"; CPUS="${WUP_QEMU_CPUS:-4}";;
    zero2w) CPU=cortex-a53; MEM="${WUP_QEMU_MEM:-512M}"; CPUS="${WUP_QEMU_CPUS:-4}";;
    *) echo "unknown profile $PROFILE" >&2; exit 2;;
esac

HADDR="${WUP_QEMU_HOST_ADDR:-0.0.0.0}"
HPORT="${WUP_QEMU_HOST_PORT:-5080}"
GPORT="${WUP_QEMU_GUEST_PORT:-50001}"
SPORT="${WUP_QEMU_SSH_PORT:-2222}"
FWD="udp:${HADDR}:${HPORT}-:${GPORT}"
[ "$SPORT" != "0" ] && FWD="$FWD,hostfwd=tcp:${HADDR}:${SPORT}-:22"

# firmware: prefer AAVMF pflash pair, else the combined QEMU_EFI.fd
if [ -f /usr/share/AAVMF/AAVMF_CODE.fd ]; then
    CODE=/usr/share/AAVMF/AAVMF_CODE.fd; VARS=/usr/share/AAVMF/AAVMF_VARS.fd
    cp -n "$VARS" "$RUNDIR/AAVMF_VARS.fd" 2>/dev/null || true
    BIOS_ARGS=(-drive if=pflash,format=raw,unit=0,readonly=on,file="$CODE"
               -drive if=pflash,format=raw,unit=1,file="$RUNDIR/AAVMF_VARS.fd")
else
    BIOS_ARGS=(-bios /usr/share/qemu-efi-aarch64/QEMU_EFI.fd)
fi

# host port conflict check
if command -v ss >/dev/null && ss -ulpn 2>/dev/null | grep -q ":$HPORT "; then
    echo "warning: UDP port $HPORT already in use on the host" >&2
fi

exec qemu-system-aarch64 \
    -machine virt,gic-version=3 \
    -cpu "$CPU" -smp "$CPUS" -m "$MEM" \
    "${BIOS_ARGS[@]}" \
    -drive if=virtio,format=qcow2,file="$IMG" \
    -drive if=virtio,format=raw,media=cdrom,file="$SEED" \
    -netdev user,id=n0,hostfwd=$FWD \
    -device virtio-net-pci,netdev=n0 \
    -display none \
    -chardev "socket,id=ser0,path=$RUNDIR/serial.sock,server=on,wait=off,logfile=$SERIAL" \
    -serial chardev:ser0 \
    -monitor "unix:$MON,server,nowait" \
    -qmp "unix:$QMP,server,nowait" \
    -pidfile "$PIDFILE" \
    -name wup-thinserver-qemu
