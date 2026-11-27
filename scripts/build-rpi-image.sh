#!/usr/bin/env bash
# Build flashable Raspberry Pi OS/Debian 13 ARM64 ThinServer images with the
# official rpi-image-gen device layers and the shared native provisioning.
set -euo pipefail

# Debian installs several image-building tools (mkdosfs, sfdisk, losetup)
# in /usr/sbin, which is not always part of a non-login user's PATH.
export PATH="/usr/sbin:/sbin:$PATH"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROFILE="${1:-pi4}"
shift || true
PREPARE_ONLY=0
if [ "${1:-}" = "--prepare-only" ]; then PREPARE_ONLY=1; shift; fi
[ $# -eq 0 ] || { echo "usage: $0 pi4|zero2w [--prepare-only]" >&2; exit 2; }
case "$PROFILE" in pi4|zero2w) ;; *) echo "unknown profile: $PROFILE" >&2; exit 2;; esac

RPI_IMAGE_GEN_TAG=v2.8.0
RPI_IMAGE_GEN_COMMIT=262d4df5a9f9d4133370465399a7958a7c22cdc7
BUILD_ROOT="$ROOT/build/rpi-image-gen"
WORKROOT="$BUILD_ROOT/work-$PROFILE"
SOURCE="$BUILD_ROOT/source"
OUTDIR="$ROOT/build/images"
CONFIG="thinserver-$PROFILE.yaml"
IMAGE_NAME="wup-thinserver-$PROFILE-arm64"

log() { printf '\033[1;36m[rpi-image]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[rpi-image] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

mkdir -p "$BUILD_ROOT" "$OUTDIR"
PROJECT="$(mktemp -d "$BUILD_ROOT/project-$PROFILE.XXXXXX")"
if [ ! -d "$SOURCE/.git" ]; then
    log "cloning rpi-image-gen $RPI_IMAGE_GEN_TAG"
    git clone --depth 1 --branch "$RPI_IMAGE_GEN_TAG" \
        https://github.com/raspberrypi/rpi-image-gen.git "$SOURCE"
fi
[ "$(git -C "$SOURCE" rev-parse HEAD)" = "$RPI_IMAGE_GEN_COMMIT" ] ||
    die "rpi-image-gen source does not match pinned $RPI_IMAGE_GEN_COMMIT"

for command in dpkg-architecture git ssh-keygen xz sha256sum; do
    command -v "$command" >/dev/null || die "missing build tool: $command"
done

log "building the shared ARM64 application payload"
"$ROOT/scripts/build-arm64.sh" "$ROOT/build-arm64"

mkdir -p "$PROJECT/config" "$PROJECT/layer" \
    "$PROJECT/layer/wup-thinserver.rootfs-overlay/opt/wup" \
    "$PROJECT/layer/wup-thinserver.rootfs-overlay/opt/wup/source/scripts" \
    "$PROJECT/layer/wup-thinserver.rootfs-overlay/opt/wup/source/packaging/systemd" \
    "$PROJECT/layer/wup-thinserver.rootfs-overlay/opt/wup/prebuilt"
cp "$ROOT/images/raspberry-pi/config/$CONFIG" "$PROJECT/config/$CONFIG"
cp "$ROOT/images/raspberry-pi/layer/wup-thinserver.yaml" "$PROJECT/layer/"
cp "$ROOT/images/raspberry-pi/provision-rpi-image.sh" \
    "$PROJECT/layer/wup-thinserver.rootfs-overlay/opt/wup/"
chmod 0755 "$PROJECT/layer/wup-thinserver.rootfs-overlay/opt/wup/provision-rpi-image.sh"
printf '%s\n' "$PROFILE" > "$PROJECT/layer/wup-thinserver.rootfs-overlay/opt/wup/profile"
cp -a "$ROOT/build-arm64/install-root/usr" \
    "$PROJECT/layer/wup-thinserver.rootfs-overlay/opt/wup/prebuilt/"
cp "$ROOT/scripts/install-native.sh" \
    "$PROJECT/layer/wup-thinserver.rootfs-overlay/opt/wup/source/scripts/"
chmod 0755 "$PROJECT/layer/wup-thinserver.rootfs-overlay/opt/wup/source/scripts/install-native.sh"
cp "$ROOT/packaging/systemd/wup-thinserver.service" \
    "$PROJECT/layer/wup-thinserver.rootfs-overlay/opt/wup/source/packaging/systemd/"

KEY="$OUTDIR/rpi-admin-key"
if [ ! -f "$KEY" ]; then
    ssh-keygen -q -t ed25519 -N '' -C 'wup-rpi-admin' -f "$KEY"
    log "generated locked-image admin key: $KEY"
fi
cp "$KEY.pub" "$PROJECT/admin-key.pub"

# Package selection remains sourced from provisioning/, exactly like Docker,
# native Debian and the QEMU appliance. The Zero 2 W list is intentionally
# smaller instead of merely constraining the full Pi 4 image to 512 MiB.
PACKAGE_FILES=("$ROOT/provisioning/packages/runtime.txt")
if [ "$PROFILE" = zero2w ]; then
    PACKAGE_FILES+=("$ROOT/provisioning/packages/desktop-zero2w.txt")
else
    PACKAGE_FILES+=("$ROOT/provisioning/packages/desktop.txt")
fi
{
    echo
    echo "packages:"
    grep -hEv '^\s*(#|$)' "${PACKAGE_FILES[@]}" | sort -u | sed 's/^/  - /'
} >> "$PROJECT/config/$CONFIG"

log "prepared pinned project at $PROJECT"
if [ "$PREPARE_ONLY" = 1 ]; then
    log "prepare-only complete"
    exit 0
fi

for command in mmdebstrap qemu-aarch64-static; do
    command -v "$command" >/dev/null ||
        die "missing rpi-image-gen dependency: $command; run $SOURCE/install_deps.sh as documented upstream"
done

log "building $PROFILE image (this requires mount-namespace privileges)"
(
    # rpi-image-gen resolves work/ from the caller's current directory.
    # Keep the large chroots, raw images and SBOMs under the ignored build/.
    # Each profile gets its own workroot (-B) so pi4 and zero2w builds can
    # run in parallel without colliding on chroot-c6e710e-dirty/.
    cd "$BUILD_ROOT"
    mkdir -p "$WORKROOT"
    # Its host-tool build installs shared libraries beside genimage without an
    # rpath. Keep that private prefix visible on both AMD64 and ARM64 hosts.
    HOST_TRIPLET="$(dpkg-architecture -qDEB_BUILD_GNU_TYPE)"
    export LD_LIBRARY_PATH="$WORKROOT/$HOST_TRIPLET/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    "$SOURCE/rpi-image-gen" build -B "$WORKROOT" -S "$PROJECT" -c "$PROJECT/config/$CONFIG"
)
IMAGE="$WORKROOT/image-$IMAGE_NAME/$IMAGE_NAME.img"
[ -f "$IMAGE" ] || die "rpi-image-gen did not produce $IMAGE"
OUTPUT="$OUTDIR/$IMAGE_NAME.img.xz"
xz -T0 -9 -c "$IMAGE" > "$OUTPUT"
sha256sum "$OUTPUT" > "$OUTPUT.sha256"
log "image: $OUTPUT"
log "checksum: $(cat "$OUTPUT.sha256")"

