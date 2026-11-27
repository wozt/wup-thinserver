#!/usr/bin/env bash
# Early-boot smoke test for the generated Pi 4 image. This validates the
# Raspberry Pi kernel, root filesystem and systemd integration only; QEMU's
# incomplete VideoCore and network models cannot validate physical hardware.
set -euo pipefail

export PATH="/usr/sbin:/sbin:$PATH"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TIMEOUT="${1:-90}"
IMAGE_DIR="$ROOT/build/rpi-image-gen/work/image-wup-thinserver-pi4-arm64"
IMAGE="$IMAGE_DIR/wup-thinserver-pi4-arm64.img"
BOOT="$IMAGE_DIR/boot.vfat"

case "$TIMEOUT" in
    ''|*[!0-9]*) echo "usage: $0 [timeout-seconds]" >&2; exit 2 ;;
esac
[ "$TIMEOUT" -gt 0 ] || { echo "timeout must be positive" >&2; exit 2; }

for command in mcopy qemu-img qemu-system-aarch64 timeout grep; do
    command -v "$command" >/dev/null || {
        echo "missing smoke-test tool: $command" >&2
        exit 1
    }
done
[ -f "$IMAGE" ] && [ -f "$BOOT" ] || {
    echo "build the Pi 4 image before running this test" >&2
    exit 1
}
qemu-system-aarch64 -machine help | grep -q '^raspi4b ' || {
    echo "this QEMU build has no raspi4b machine" >&2
    exit 1
}

mkdir -p "$ROOT/build"
TEMP="$(mktemp -d "$ROOT/build/rpi-qemu-smoke.XXXXXX")"
cleanup() {
    rm -f "$TEMP/kernel8.img" "$TEMP/initramfs8" \
        "$TEMP/bcm2711-rpi-4-b.dtb" "$TEMP/sd.qcow2"
    rmdir "$TEMP" 2>/dev/null || true
}
trap cleanup EXIT

for asset in kernel8.img initramfs8 bcm2711-rpi-4-b.dtb; do
    mcopy -i "$BOOT" "::$asset" "$TEMP/$asset"
done

# raspi4b requires a power-of-two SD capacity. The 16 GiB copy-on-write
# overlay leaves the generated 8.26 GiB image untouched.
qemu-img create -q -f qcow2 -F raw -b "$IMAGE" "$TEMP/sd.qcow2" 16G

set +e
OUTPUT="$(timeout "${TIMEOUT}s" qemu-system-aarch64 \
    -M raspi4b -m 2G -display none -serial stdio -monitor none -nic none \
    -kernel "$TEMP/kernel8.img" \
    -dtb "$TEMP/bcm2711-rpi-4-b.dtb" \
    -initrd "$TEMP/initramfs8" \
    -append 'console=ttyAMA1,115200 earlycon=pl011,0xfe201000 root=/dev/mmcblk1p2 rootfstype=ext4 rw rootwait systemd.unit=multi-user.target systemd.mask=boot-firmware.mount systemd.mask=systemd-networkd-wait-online.service' \
    -drive "file=$TEMP/sd.qcow2,if=sd,format=qcow2,snapshot=on" 2>&1)"
STATUS=$?
set -e
printf '%s\n' "$OUTPUT"

if [ "$STATUS" -ne 0 ] && [ "$STATUS" -ne 124 ]; then
    echo "QEMU exited unexpectedly with status $STATUS" >&2
    exit "$STATUS"
fi
grep -q 'wup-thinserver.service' <<<"$OUTPUT" || {
    echo "QEMU did not reach wup-thinserver.service within ${TIMEOUT}s" >&2
    exit 1
}
grep -q 'multi-user.target' <<<"$OUTPUT" || {
    echo "QEMU did not reach multi-user.target within ${TIMEOUT}s" >&2
    exit 1
}
echo "Pi 4 raspi4b early-boot smoke test passed"
