#!/usr/bin/env bash
# build-qemu-image.sh — build the bootable ARM64 QEMU ThinServer image.
#
# Produces:
#   build/images/wup-thinserver-qemu-arm64.qcow2   self-contained disk image
#   build/images/seed.img                          cloud-init seed (attached at boot)
#   build/images/qemu-admin-key(+ .pub)            ephemeral SSH admin keypair
#
# Base: Debian 13 (trixie) genericcloud arm64, pinned by SHA512.
# First boot runs cloud-init -> /opt/wup/provision-guest.sh which installs
# the desktop stack and enables the ThinServer systemd unit.
#
# Usage: scripts/build-qemu-image.sh [--rebuild] [--skip-arm64-build]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMGDIR="$ROOT/build/images"
BASE_DIR="$IMGDIR/base"
OUT="$IMGDIR/wup-thinserver-qemu-arm64.qcow2"
SEED="$IMGDIR/seed.img"
SEEDDIR="$(mktemp -d)"
trap 'rm -rf "$SEEDDIR"' EXIT

BASE_URL="https://cloud.debian.org/images/cloud/trixie/latest/debian-13-genericcloud-arm64.qcow2"
BASE_NAME="debian-13-genericcloud-arm64.qcow2"
# sha512 of the pinned base image (from upstream SHA512SUMS, trixie/latest).
BASE_SHA512="3942cc527cb142f1cd8f5382b6411e9fe5e8f06b8f33cc2800faef9dff658fbdec4f0ac395a4eae02e8e1706f9dd71224ebbee8d5c5f763773d39e75a1321d81"
DISK_SIZE="20G"

REBUILD=0; SKIP_ARM64=0
for a in "$@"; do case "$a" in
    --rebuild) REBUILD=1;; --skip-arm64-build) SKIP_ARM64=1;; *) echo "unknown arg $a" >&2; exit 2;;
esac; done

log(){ printf '\033[1;36m[qemu-img]\033[0m %s\n' "$*"; }
die(){ printf '\033[1;31m[qemu-img] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

for t in qemu-img qemu-system-aarch64 cloud-localds xorriso curl sha512sum; do
    command -v "$t" >/dev/null || die "missing tool: $t (apt install qemu-system-arm qemu-utils cloud-image-utils xorriso curl)"
done
[ -f /usr/share/qemu-efi-aarch64/QEMU_EFI.fd ] || [ -f /usr/share/AAVMF/AAVMF_CODE.fd ] \
    || die "missing aarch64 UEFI firmware (apt install qemu-efi-aarch64)"

mkdir -p "$BASE_DIR" "$IMGDIR"

# ---------- base image -------------------------------------------------------
BASE="$BASE_DIR/$BASE_NAME"
if [ ! -f "$BASE" ]; then
    log "downloading $BASE_NAME"
    curl -fL --retry 3 -o "$BASE" "$BASE_URL"
fi
echo "$BASE_SHA512  $BASE" | sha512sum -c - >/dev/null \
    || die "base image checksum mismatch (update BASE_SHA512 if upstream rotated)"
log "base image verified ($(du -h "$BASE" | cut -f1))"

# ---------- arm64 payload ------------------------------------------------------
INSTALL_ROOT="$ROOT/build-arm64/install-root"
if [ "$SKIP_ARM64" = 0 ] || [ ! -x "$INSTALL_ROOT/usr/local/bin/wup-thinserver" ]; then
    log "cross-compiling arm64 binaries"
    "$ROOT/scripts/build-arm64.sh" "$ROOT/build-arm64" >/dev/null
fi
[ -x "$INSTALL_ROOT/usr/local/bin/wup-thinserver" ] || die "arm64 build missing"

PAYLOAD_STAGE="$(mktemp -d)"
trap 'rm -rf "$SEEDDIR" "$PAYLOAD_STAGE"' EXIT
mkdir -p "$PAYLOAD_STAGE/usr" "$PAYLOAD_STAGE/etc/systemd/system" \
         "$PAYLOAD_STAGE/opt/wup/packages"
cp -a "$INSTALL_ROOT/usr/local" "$PAYLOAD_STAGE/usr/"
install -m 644 "$ROOT/packaging/systemd/wup-thinserver.service" \
    "$PAYLOAD_STAGE/etc/systemd/system/wup-thinserver.service"
cp "$ROOT/provisioning/packages/runtime.txt" "$ROOT/provisioning/packages/desktop.txt" \
   "$PAYLOAD_STAGE/opt/wup/packages/"
install -m 755 "$ROOT/images/qemu/provision-guest.sh" "$PAYLOAD_STAGE/opt/wup/provision-guest.sh"
# NOTE: never archive "." — mktemp -d is mode 700 and tar would chmod 700
# the guest "/" on extraction. List top-level dirs explicitly and force
# root ownership so host uids don't leak into the image.
tar --owner=0 --group=0 -C "$PAYLOAD_STAGE" -czf "$SEEDDIR/payload.tar.gz" \
    usr etc opt
cp "$SEEDDIR/payload.tar.gz" "$IMGDIR/qemu-arm64-payload.tar.gz"
log "payload: $(du -h "$SEEDDIR/payload.tar.gz" | cut -f1)"

# ---------- admin SSH key -------------------------------------------------------
KEY="$IMGDIR/qemu-admin-key"
if [ ! -f "$KEY" ]; then
    ssh-keygen -q -t ed25519 -N '' -C 'wup-qemu-admin' -f "$KEY"
    log "generated admin SSH key: $KEY(.pub)"
fi
PUB="$(cat "$KEY.pub")"

# ---------- cloud-init seed -------------------------------------------------------
cat > "$SEEDDIR/user-data" <<EOF
#cloud-config
hostname: wup-thinserver-arm64
manage_etc_hosts: true
users:
  - name: admin
    gecos: WUP ThinServer admin
    groups: [sudo]
    shell: /bin/bash
    sudo: ALL=(ALL) NOPASSWD:ALL
    ssh_authorized_keys:
      - ${PUB}
ssh_pwauth: false
package_update: false
# No runcmd: the image is fully provisioned OFFLINE by
# scripts/provision-qemu-image.sh (qemu-nbd + arm64 chroot) before first
# boot, so cloud-init only sets identity/SSH here — first boot takes
# seconds instead of an hour of TCG apt.
EOF
cat > "$SEEDDIR/meta-data" <<'EOF'
instance-id: wup-thinserver-qemu-arm64-001
local-hostname: wup-thinserver-arm64
EOF

log "building seed image"
xorriso -as mkisofs -quiet -V cidata -J -R -o "$SEED" \
    "$SEEDDIR/user-data" "$SEEDDIR/meta-data" 2>/dev/null \
    || xorrisofs -o "$SEED" -V cidata -J -R \
       "$SEEDDIR/user-data" "$SEEDDIR/meta-data"

# ---------- final disk image -------------------------------------------------------
if [ "$REBUILD" = 1 ] || [ ! -f "$OUT" ]; then
    log "creating $OUT ($DISK_SIZE)"
    qemu-img convert -O qcow2 "$BASE" "$OUT"
    qemu-img resize "$OUT" "$DISK_SIZE"
else
    log "reusing existing $OUT (--rebuild to recreate)"
fi

# ---------- offline provisioning ----------------------------------------------------
# Mount via qemu-nbd and run provision-guest.sh in an arm64 chroot
# (qemu-aarch64-static binfmt) — ~10x faster than first-boot apt under TCG.
if [ "${SKIP_PROVISION:-0}" != "1" ]; then
    "$ROOT/scripts/provision-qemu-image.sh" "$OUT" "$IMGDIR/qemu-arm64-payload.tar.gz"
fi
log "image: $OUT ($(du -h "$OUT" | cut -f1))"
log "next: ./scripts/run-qemu.sh"
