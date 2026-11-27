#!/usr/bin/env bash
# provision-qemu-image.sh — provision the ARM64 guest image OFFLINE.
#
# Mounts the qcow2 through qemu-nbd and runs the same
# images/qemu/provision-guest.sh inside an arm64 chroot. arm64 binaries
# execute through the registered qemu-aarch64-static binfmt handler, which
# is far faster than booting the full system under TCG — the first boot of
# the resulting image then only needs cloud-init identity setup (seconds).
#
# Usage: provision-qemu-image.sh <image.qcow2> [payload.tar.gz]
#   payload defaults to the tarball built by build-qemu-image.sh.
# The VM must NOT be running on the image (qemu-img lock).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${1:?usage: $0 <image.qcow2> [payload.tar.gz]}"
PAYLOAD="${2:-$ROOT/build/images/qemu-arm64-payload.tar.gz}"
NBD=/dev/nbd0
MNT=/mnt/wup-qemu-prov

[ -f "$IMG" ] || { echo "missing image $IMG" >&2; exit 1; }
[ -f "$PAYLOAD" ] || { echo "missing payload $PAYLOAD — run scripts/build-qemu-image.sh" >&2; exit 1; }
command -v qemu-nbd >/dev/null || { echo "qemu-nbd required" >&2; exit 1; }

cleanup() {
    sudo umount -R "$MNT" 2>/dev/null || true
    sudo qemu-nbd --disconnect "$NBD" 2>/dev/null || true
}
trap cleanup EXIT

log(){ echo "[provision-qemu] $*"; }

sudo modprobe nbd max_part=8 2>/dev/null || true
sudo qemu-nbd --connect="$NBD" --format=qcow2 "$IMG"
sleep 1
sudo partprobe "$NBD" 2>/dev/null || true
sleep 1
# genericcloud arm64: first partition is the rootfs
ROOTPART="$(lsblk -lnpo NAME "$NBD" | sed -n '2p')"
[ -n "$ROOTPART" ] || { echo "cannot find root partition on $NBD" >&2; exit 1; }
sudo mkdir -p "$MNT"
sudo mount -o rw "$ROOTPART" "$MNT"

# binfmt interpreter must be reachable inside the chroot
sudo cp "$(command -v qemu-aarch64-static)" "$MNT/usr/bin/" 2>/dev/null || \
    sudo cp /usr/bin/qemu-aarch64-static "$MNT/usr/bin/"
# bind virtual filesystems + network config for apt
for d in dev proc sys run; do sudo mount --bind "/$d" "$MNT/$d"; done
# resolv.conf in the cloud image is a dangling symlink inside the chroot —
# replace it with a real file (cloud-init regenerates it on first boot)
sudo rm -f "$MNT/etc/resolv.conf"
cat /etc/resolv.conf | sudo tee "$MNT/etc/resolv.conf" >/dev/null

# unpack payload (binaries, systemd unit, package lists, provision script)
sudo tar -xzf "$PAYLOAD" -C "$MNT"

log "running provision-guest.sh in arm64 chroot (qemu-user emulation)"
sudo chroot "$MNT" /bin/bash /opt/wup/provision-guest.sh

# hostname for dedicated appliance (cloud-init also sets it; harmless)
echo wup-thinserver-arm64 | sudo tee "$MNT/etc/hostname" >/dev/null

# unmount in reverse order
for d in run sys proc dev; do sudo umount "$MNT/$d" 2>/dev/null || true; done
sudo umount "$MNT"
cleanup
log "done — image $IMG is provisioned; first boot is fast (no apt)"
