#!/bin/bash
# Runs inside the pi-gen chroot for Raspberry Pi OS (Trixie) images.
# Counterpart of provision-rpi-image.sh for the Debian images, minus the
# iwd/wup-firstboot Wi-Fi plumbing: Raspberry Pi OS uses cloud-init, so
# Raspberry Pi Imager OS customization (user-data / network-config) handles
# Wi-Fi, users and hostname natively on first boot.
set -euo pipefail

PROFILE="$(cat /opt/wup/profile)"
case "$PROFILE" in
    pi4) WIDTH=1280; HEIGHT=720; PORT=50004; DOCK=190 ;;
    zero2w) WIDTH=854; HEIGHT=480; PORT=50005; DOCK=145 ;;
    *) echo "unknown Raspberry Pi OS profile: $PROFILE" >&2; exit 2 ;;
esac

# Shared native installer owns accounts (baseline wup/wup + sudo + root/wup),
# settings, prebuilt payload and the systemd unit. Package lists come from
# provisioning/packages/ exactly like every other deployment path.
/opt/wup/scripts/install-native.sh \
    --prebuilt /opt/wup/prebuilt \
    --desktop "${WIDTH}x${HEIGHT}" \
    --port "$PORT" \
    --no-start

# Raspberry Pi OS keeps password SSH disabled by default (stage2 sys-tweaks);
# the appliance baseline expects ssh wup@<ip> to work out of the box.
install -d -m 0755 /etc/ssh/sshd_config.d
printf 'PasswordAuthentication yes\n' \
    > /etc/ssh/sshd_config.d/60-wup-thinserver.conf

# uinput for the virtual GamePad, same as the Debian images.
groupadd -f input
usermod -aG input,video,audio wup
install -d -m 0755 /etc/udev/rules.d
cat > /etc/udev/rules.d/70-wup-thinserver.rules <<'EOF'
KERNEL=="uinput", GROUP="input", MODE="0660"
KERNEL=="video[0-9]*", ATTR{name}=="Wii U GamePad Camera", GROUP="video", MODE="0660"
EOF
printf '%s\n' uinput > /etc/modules-load.d/wup-thinserver-gamepad.conf

# No physical desktop or display manager; console and SSH remain usable.
systemctl mask display-manager.service labwc.service wayfire.service 2>/dev/null || true

touch /var/lib/wup-thinserver-pios-image
