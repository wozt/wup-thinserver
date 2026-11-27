#!/usr/bin/env bash
# register-libvirt.sh — register the existing ARM64 QEMU appliance with
# libvirt (qemu:///system) so it can be managed from Virt-Manager.
#
# Reuses the already-built artifacts — nothing is rebuilt or reinstalled:
#   build/images/wup-thinserver-qemu-arm64.qcow2  (guest disk, all data kept)
#   build/images/seed.img                        (cloud-init cidata seed)
#   build/run/AAVMF_VARS.fd                      (UEFI variables, copied)
#   build/images/qemu-admin-key                  (SSH admin key, unchanged)
#
# The VM keeps: aarch64 TCG, cortex-a72, 4 vCPU, 4 GiB, virtio disk/net,
# bridged networking on br0 (guest gets its own LAN IP via DHCP),
# headless serial console + VNC on 127.0.0.1.
#
# Usage:
#   ./scripts/register-libvirt.sh          # define (or update) + start
#   ./scripts/register-libvirt.sh --stop   # graceful shutdown via ACPI
#   ./scripts/register-libvirt.sh --undefine  # remove domain (disk kept)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NAME=wup-thinserver-arm64
IMG="$ROOT/build/images/wup-thinserver-qemu-arm64.qcow2"
SEED="$ROOT/build/images/seed.img"
MEM_MIB="${WUP_QEMU_MEM_MIB:-8192}"   # TCG guest: more RAM eases desktop pressure
VCPUS="${WUP_QEMU_VCPUS:-6}"          # encode is single-threaded; extra vCPUs help capture/desktop
VARS_TPL=/usr/share/AAVMF/AAVMF_VARS.fd
CODE=/usr/share/AAVMF/AAVMF_CODE.fd
VARS="$ROOT/build/run/libvirt-AAVMF_VARS.fd"
SERIAL_LOG="$ROOT/build/run/serial-libvirt.log"
CONSOLE_LOG="$ROOT/build/run/serial-libvirt-console.log"
XML="$ROOT/build/run/$NAME.xml"
URI=qemu:///system

log(){ echo "[libvirt] $*"; }
die(){ echo "[libvirt] ERROR: $*" >&2; exit 1; }

[ -f "$IMG" ]  || die "missing disk image $IMG — run scripts/build-qemu-image.sh"
[ -f "$SEED" ] || die "missing seed $SEED — run scripts/build-qemu-image.sh"
[ -f "$CODE" ] || die "missing AAVMF firmware $CODE — apt install qemu-efi-aarch64"
command -v virsh >/dev/null || die "virsh not found — apt install libvirt-clients"
virsh -c "$URI" list >/dev/null 2>&1 || die "cannot reach $URI (libvirt-daemon running? user in libvirt group?)"

state() { virsh -c "$URI" domstate "$NAME" 2>/dev/null || echo "shut off"; }

case "${1:-}" in
    --stop)
        [ "$(state)" = "running" ] && virsh -c "$URI" shutdown "$NAME" || true
        log "shutdown requested for $NAME"; exit 0;;
    --undefine)
        virsh -c "$URI" destroy "$NAME" 2>/dev/null || true
        virsh -c "$URI" undefine "$NAME" --nvram 2>/dev/null || \
            virsh -c "$URI" undefine "$NAME"
        log "domain $NAME removed (disk image untouched)"; exit 0;;
    ""|--start) ;;
    *) die "unknown arg $1";;
esac

# A standalone run-qemu.sh instance must not hold the disk at the same time.
if pgrep -f "qemu-system-aarch64.*$IMG" >/dev/null; then
    die "standalone QEMU is already using $IMG — run scripts/stop-qemu.sh first"
fi

# UEFI variables: one writable copy per VM (guest writes boot entries here).
[ -f "$VARS" ] || { cp "$VARS_TPL" "$VARS"; log "created UEFI vars $VARS"; }

mkdir -p "$ROOT/build/run"
cat > "$XML" <<EOF
<domain type='qemu'>   <!-- TCG: no ARM64 KVM on this x86_64 host -->
  <name>$NAME</name>
  <memory unit='KiB'>$((MEM_MIB * 1024))</memory>
  <currentMemory unit='KiB'>$((MEM_MIB * 1024))</currentMemory>
  <vcpu placement='static'>$VCPUS</vcpu>
  <os>
    <type arch='aarch64' machine='virt'>hvm</type>
    <loader readonly='yes' type='pflash'>$CODE</loader>
    <nvram template='$VARS_TPL'>$VARS</nvram>
    <boot dev='hd'/>
  </os>
  <cpu mode='custom' match='exact'>
    <model>cortex-a72</model>
    <topology sockets='1' dies='1' clusters='1' cores='$VCPUS' threads='1'/>
  </cpu>
  <features>
    <acpi/>
    <apic/>
  </features>
  <clock offset='utc'/>
  <on_poweroff>destroy</on_poweroff>
  <on_reboot>restart</on_reboot>
  <on_crash>destroy</on_crash>
  <devices>
    <emulator>/usr/bin/qemu-system-aarch64</emulator>
    <disk type='file' device='disk'>
      <driver name='qemu' type='qcow2' cache='none' io='native'/>
      <source file='$IMG'/>
      <target dev='vda' bus='virtio'/>
    </disk>
    <!-- cloud-init NoCloud seed: any block device labeled 'cidata' works,
         so a plain virtio disk is fine (no cdrom bus needed) -->
    <disk type='file' device='disk'>
      <driver name='qemu' type='raw' cache='none' io='native'/>
      <source file='$SEED'/>
      <target dev='vdb' bus='virtio'/>
    </disk>
    <controller type='virtio-serial' index='0'/>
    <serial type='file'>
      <source path='$SERIAL_LOG'/>
      <target port='0'/>
    </serial>
    <console type='pty'>
      <target type='serial' port='0'/>
    </console>
    <!-- headless: VNC bound to localhost only (optional console access);
         the streamed desktop is the Xvfb session inside the guest -->
    <graphics type='vnc' port='-1' autoport='yes'>
      <listen type='address' address='127.0.0.1'/>
    </graphics>
    <video><model type='virtio'/></video>
    <memballoon model='none'/>
    <!-- Bridged networking: the guest gets its own LAN IP via DHCP on
         br0 (host bridge, enp42s0 enslaved). No NAT, no hostfwd needed —
         the GamePad connects directly to <guest-ip>:<listen_port> and
         SSH goes to <guest-ip>:22 with build/images/qemu-admin-key. -->
    <interface type='bridge'>
      <source bridge='br0'/>
      <mac address='52:54:00:12:34:56'/>
      <model type='virtio'/>
    </interface>
  </devices>
</domain>
EOF

# Re-defining a domain that has an nvram file is rejected by libvirt, so
# an existing (shut-off) domain is undef'd first; the nvram file itself
# stays on disk at its fixed path, preserving guest boot variables.
if virsh -c "$URI" dominfo "$NAME" >/dev/null 2>&1; then
    if [ "$(state)" != "running" ]; then
        virsh -c "$URI" undefine "$NAME" --nvram 2>/dev/null || \
            virsh -c "$URI" undefine "$NAME"
        log "removed previous domain definition (nvram file kept)"
    else
        die "domain $NAME is running — stop it first to update the definition"
    fi
fi
log "defining domain $NAME"
virsh -c "$URI" define "$XML" >/dev/null

case "$(state)" in
    running) log "domain already running";;
    *)       log "starting domain"; virsh -c "$URI" start "$NAME" >/dev/null;;
esac

log "domain $NAME is $(state) — visible in Virt-Manager (connection qemu:///system)"
log "serial log: $SERIAL_LOG   (virsh console $NAME for interactive serial)"
log "verify:     ./scripts/qemu-smoke.sh"
