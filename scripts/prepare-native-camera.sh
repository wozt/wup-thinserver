#!/usr/bin/env bash
# Prepare one dedicated v4l2loopback device for native ThinServer installs.
# Package installation is explicit (--install); ordinary service starts never
# modify the running kernel or install host packages.
set -euo pipefail

usage() {
    echo "usage: $0 [--install] [--video-number N]"
}

INSTALL=0; REQUESTED=42
while [ $# -gt 0 ]; do case "$1" in
    --install) INSTALL=1; shift ;;
    --video-number)
        [ $# -ge 2 ] || { usage >&2; exit 2; }
        REQUESTED="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
esac; done
[[ "$REQUESTED" =~ ^[0-9]+$ ]] || { echo "video number must be numeric" >&2; exit 2; }
[ "$REQUESTED" -le 255 ] || { echo "video number must be at most 255" >&2; exit 2; }
[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }

if [ "$INSTALL" = 1 ]; then
    apt-get update
    if [ -r /etc/rpi-issue ] || grep -qi raspberry /proc/device-tree/model 2>/dev/null; then
        # Raspberry Pi OS needs headers matching its downstream kernel. Do not
        # substitute generic Debian headers for a Raspberry Pi kernel.
        apt-get install -y linux-headers-rpi-v8 v4l2loopback-dkms v4l2loopback-utils
    else
        apt-get install -y "linux-headers-$(uname -r)" v4l2loopback-dkms v4l2loopback-utils
    fi
fi
modinfo v4l2loopback >/dev/null 2>&1 || {
    echo "v4l2loopback is unavailable; rerun with --install after matching kernel headers are installed" >&2
    exit 1
}

NUMBER="$REQUESTED"
while [ -e "/dev/video$NUMBER" ]; do
    NAME="$(cat "/sys/class/video4linux/video$NUMBER/name" 2>/dev/null || true)"
    if [ "$NAME" = "Wii U GamePad Camera" ]; then
        echo "/dev/video$NUMBER already is the dedicated Wii U GamePad Camera"
        exit 0
    fi
    NUMBER=$((NUMBER + 1))
    [ "$NUMBER" -le 255 ] || {
        echo "no free /dev/video number is available" >&2
        exit 1
    }
done

# Loading the module again cannot add a device. Never unload an existing
# v4l2loopback instance because another application may own its devices.
if grep -q '^v4l2loopback ' /proc/modules; then
    echo "v4l2loopback is already loaded without a Wii U GamePad Camera; refusing to disrupt existing virtual cameras" >&2
    exit 1
fi

cat > /etc/modprobe.d/wup-thinserver-camera.conf <<EOF
options v4l2loopback video_nr=$NUMBER card_label="Wii U GamePad Camera" exclusive_caps=1
EOF
printf '%s\n' v4l2loopback > /etc/modules-load.d/wup-thinserver-camera.conf
modprobe v4l2loopback video_nr="$NUMBER" card_label="Wii U GamePad Camera" exclusive_caps=1
NAME="$(cat "/sys/class/video4linux/video$NUMBER/name" 2>/dev/null || true)"
[ "$NAME" = "Wii U GamePad Camera" ] || {
    echo "created device identity is incorrect: $NAME" >&2; exit 1;
}
echo "dedicated camera ready at /dev/video$NUMBER"
if [ "$NUMBER" != "$REQUESTED" ]; then
    echo "requested /dev/video$REQUESTED was occupied; set camera_device to /dev/video$NUMBER"
fi

