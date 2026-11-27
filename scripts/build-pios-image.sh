#!/usr/bin/env bash
# build-pios-image.sh — build Raspberry Pi OS (Trixie) ThinServer images
# with pi-gen, the official Raspberry Pi OS image tool.
#
# Unlike the Debian images (rpi-image-gen + iwd + wup-firstboot.conf),
# Raspberry Pi OS ships cloud-init: Raspberry Pi Imager OS customization
# (user-data / network-config / firstrun.sh on the BOOT partition) works
# natively, including Wi-Fi provisioning — no credentials file needed.
#
# Usage: scripts/build-pios-image.sh pi4|zero2w
#
# Runs on aarch64 hosts natively; on x86_64 set USE_QEMU=1 with
# qemu-user-static + binfmt registered (arch-test must pass).
set -euo pipefail

export PATH="/usr/sbin:/sbin:$PATH"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROFILE="${1:-pi4}"
case "$PROFILE" in pi4|zero2w) ;; *) echo "usage: $0 pi4|zero2w" >&2; exit 2;; esac

PI_GEN_BRANCH=arm64
PI_GEN_TAG=2026-10-06-raspios-trixie-arm64
BUILD_ROOT="$ROOT/build/pi-gen"
SOURCE="$BUILD_ROOT/source"
OUTDIR="$ROOT/build/images"
STAGE_NAME="wup-thinserver"

log() { printf '\033[1;36m[pios-image]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[pios-image] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

mkdir -p "$BUILD_ROOT" "$OUTDIR"
if [ ! -d "$SOURCE/.git" ]; then
    log "cloning pi-gen ($PI_GEN_BRANCH)"
    git clone --depth 1 --branch "$PI_GEN_BRANCH" \
        https://github.com/RPi-Distro/pi-gen.git "$SOURCE"
fi

# Desktop profile identical to the Debian Pi images: same resolution, port
# and Onboard dock height, applied through install-native.sh drop-ins.
case "$PROFILE" in
    pi4)    WIDTH=1280; HEIGHT=720; PORT=50004; DOCK=190 ;;
    zero2w) WIDTH=854;  HEIGHT=480; PORT=50005; DOCK=145 ;;
esac
IMG_NAME="wup-thinserver-pios-$PROFILE-arm64"

# --- build the shared ARM64 payload ------------------------------------------
# aarch64 host: native meson build; x86_64 host: cross toolchain.
if [ "$(uname -m)" = aarch64 ]; then
    log "building the shared ARM64 payload natively"
    PAYLOAD_BUILD="$ROOT/build-native-arm64"
    if [ ! -d "$PAYLOAD_BUILD" ]; then
        meson setup "$PAYLOAD_BUILD" --buildtype=release -Dsettings_app=enabled
    fi
    meson compile -C "$PAYLOAD_BUILD" -j"$(nproc)"
    rm -rf "$PAYLOAD_BUILD/install-root"
    meson install -C "$PAYLOAD_BUILD" --destdir "$PAYLOAD_BUILD/install-root"
    PAYLOAD_ROOT="$PAYLOAD_BUILD/install-root"
else
    log "cross-building the shared ARM64 application payload"
    "$ROOT/scripts/build-arm64.sh" "$ROOT/build-arm64" >/dev/null
    PAYLOAD_ROOT="$ROOT/build-arm64/install-root"
fi

# --- stage our custom pi-gen stage --------------------------------------------
# pi-gen discovers stages via STAGE_LIST; our stage runs after stage2
# (Raspberry Pi OS Lite + cloud-init) and exports the final image.
STAGE_DIR="$SOURCE/$STAGE_NAME"
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR/01-provision" "$STAGE_DIR/02-config"

cat > "$STAGE_DIR/prerun.sh" <<'EOF'
#!/bin/bash -e
if [ ! -d "${ROOTFS_DIR}" ]; then
    copy_previous
fi
EOF
chmod +x "$STAGE_DIR/prerun.sh"

# Host-side: stage the payload into the rootfs, then the chroot script runs
# the shared native installer exactly like the Debian Pi images.
cat > "$STAGE_DIR/01-provision/00-run.sh" <<EOF
#!/bin/bash -e
# Payload produced by scripts/build-arm64.sh + install-native.sh sources.
install -d -m 0755 "\${ROOTFS_DIR}/opt/wup/scripts"
cp -a "$PAYLOAD_ROOT" "\${ROOTFS_DIR}/opt/wup/prebuilt"
cp "$ROOT/scripts/install-native.sh" "\${ROOTFS_DIR}/opt/wup/scripts/"
chmod 0755 "\${ROOTFS_DIR}/opt/wup/scripts/install-native.sh"
cp -a "$ROOT/provisioning" "\${ROOTFS_DIR}/opt/wup/"
cp -a "$ROOT/packaging" "\${ROOTFS_DIR}/opt/wup/"
cp "$ROOT/images/raspios/provision-pios-image.sh" \
    "\${ROOTFS_DIR}/opt/wup/"
chmod 0755 "\${ROOTFS_DIR}/opt/wup/provision-pios-image.sh"
printf '%s\n' "$PROFILE" > "\${ROOTFS_DIR}/opt/wup/profile"
EOF
chmod +x "$STAGE_DIR/01-provision/00-run.sh"

cat > "$STAGE_DIR/01-provision/01-run-chroot.sh" <<'EOF'
/opt/wup/provision-pios-image.sh
EOF
chmod +x "$STAGE_DIR/01-provision/01-run-chroot.sh"

# Host-side config: enable password SSH (stage2 disables it), drop-in for
# the desktop geometry.
cat > "$STAGE_DIR/02-config/00-run.sh" <<EOF
#!/bin/bash -e
install -d -m 0755 "\${ROOTFS_DIR}/etc/ssh/sshd_config.d"
printf 'PasswordAuthentication yes\n' \
    > "\${ROOTFS_DIR}/etc/ssh/sshd_config.d/60-wup-thinserver.conf"
install -d -m 0755 "\${ROOTFS_DIR}/etc/systemd/system/wup-thinserver.service.d"
cat > "\${ROOTFS_DIR}/etc/systemd/system/wup-thinserver.service.d/20-pios.conf" <<'UNIT'
[Service]
Environment=HOME=/home/wup
Environment=DISPLAY_NUM=99
UNIT
printf 'Environment=ONBOARD_DOCK_HEIGHT=%s\n' "$DOCK" \
    >> "\${ROOTFS_DIR}/etc/systemd/system/wup-thinserver.service.d/20-pios.conf"
EOF
chmod +x "$STAGE_DIR/02-config/00-run.sh"

# Export only our stage's image: stage2's own EXPORT_IMAGE is neutralized so
# the build does not emit a bare Raspberry Pi OS Lite artifact too. Ours is
# sourced by export-image and must set IMG_SUFFIX.
rm -f "$SOURCE/stage2/EXPORT_IMAGE"
cat > "$STAGE_DIR/EXPORT_IMAGE" <<'EOF'
IMG_SUFFIX="-wup"
if [ "${USE_QEMU}" = "1" ]; then
	export IMG_SUFFIX="${IMG_SUFFIX}-qemu"
fi
EOF

# --- pi-gen config ----------------------------------------------------------------
cat > "$SOURCE/config" <<EOF
# Generated by scripts/build-pios-image.sh — do not edit.
IMG_NAME=$IMG_NAME
RELEASE=trixie
STAGE_LIST="stage0 stage1 stage2 $STAGE_NAME"
ENABLE_CLOUD_INIT=1
ENABLE_SSH=1
FIRST_USER_NAME=wup
FIRST_USER_PASS=wup
DISABLE_FIRST_BOOT_USER_RENAME=1
PASSWORDLESS_SUDO=0
TARGET_HOSTNAME=wup-thinserver-$PROFILE
DEPLOY_COMPRESSION=xz
COMPRESSION_LEVEL=9
EOF

# --- run the build ------------------------------------------------------------------
log "building $IMG_NAME (pi-gen arm64 branch, trixie)"
command -v arch-test >/dev/null || die "missing arch-test (pi-gen dependency)"
cd "$SOURCE"
./build.sh

# Locate the produced image: deploy/<date>-<IMG_NAME><suffix>.img.xz
RAW="$(find "$SOURCE/deploy" -name "*${IMG_NAME}*.img.xz" 2>/dev/null | head -1)"
[ -n "$RAW" ] || die "pi-gen did not produce an image for $IMG_NAME"
log "deployed image: $RAW"
cp "$RAW" "$OUTDIR/$(basename "$RAW")"
# Rename to the stable artifact name used by releases.
OUT="$OUTDIR/$IMG_NAME.img.xz"
mv -f "$OUTDIR/$(basename "$RAW")" "$OUT"
sha256sum "$OUT" > "$OUT.sha256"
log "image: $OUT"
log "checksum: $(cat "$OUT.sha256")"
