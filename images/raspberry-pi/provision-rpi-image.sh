#!/bin/bash
# Runs inside the rpi-image-gen chroot after the shared package lists and
# prebuilt ARM64 /usr/local tree have been installed.
set -euo pipefail

PROFILE="$(cat /opt/wup/profile)"
case "$PROFILE" in
    pi4) WIDTH=1280; HEIGHT=720; PORT=50004; DOCK=190 ;;
    zero2w) WIDTH=854; HEIGHT=480; PORT=50005; DOCK=145 ;;
    *) echo "unknown Raspberry Pi profile: $PROFILE" >&2; exit 2 ;;
esac

# The native installer remains the single owner of account creation, settings,
# prebuilt payload installation and systemd integration across Debian, QEMU and
# Raspberry Pi images. rpi-image-gen already resolved the shared package lists.
/opt/wup/source/scripts/install-native.sh \
    --prebuilt /opt/wup/prebuilt \
    --skip-packages \
    --desktop "${WIDTH}x${HEIGHT}" \
    --port "$PORT" \
    --no-start

# Baseline appliance account (wup/wup + sudo + root/wup) is set by
# install-native.sh — do NOT lock the password here.
groupadd -f input
groupadd -f video
usermod -aG input,video,audio wup

install -d -m 0755 /etc/systemd/system/wup-thinserver.service.d
cat > /etc/systemd/system/wup-thinserver.service.d/20-rpi.conf <<EOF
[Service]
Environment=HOME=/home/wup
Environment=DISPLAY_NUM=99
Environment=ONBOARD_DOCK_HEIGHT=$DOCK
EOF

install -d -m 0755 /etc/udev/rules.d
cat > /etc/udev/rules.d/70-wup-thinserver.rules <<'EOF'
KERNEL=="uinput", GROUP="input", MODE="0660"
KERNEL=="video[0-9]*", ATTR{name}=="Wii U GamePad Camera", GROUP="video", MODE="0660"
EOF
printf '%s\n' uinput > /etc/modules-load.d/wup-thinserver-gamepad.conf

# --- Pi 4 KMS/EGL session -----------------------------------------------------
# The Pi 4 replaces Xvfb with a real Xorg on the vc4 modesetting driver so
# the desktop renders into the DRM scanout; capture_backend 3 then imports
# it as DMA-BUF and scales/converts on the V3D GPU (~3x less CPU than the
# X11 path). wup-xorg.service fakes the headless HDMI connector through
# debugfs and feeds a synthetic EDID — no monitor or kernel cmdline needed.
# The Zero 2 W keeps the Xvfb session (no vc4-kms-v3d).
if [ "$PROFILE" = pi4 ]; then
    usermod -aG render wup
    install -d -m 0755 /etc/X11 /usr/lib/firmware/edid
    install -m 0644 /opt/wup/source/provisioning/session/xorg.conf.headless \
        /etc/X11/xorg.conf.headless
    install -m 0644 /opt/wup/source/provisioning/session/wup-xorg.service \
        /etc/systemd/system/wup-xorg.service
    install -m 0644 /opt/wup/source/provisioning/session/wup-edid-1280x720.bin \
        /usr/lib/firmware/edid/wup-1280x720.bin
    if [ -f /usr/local/libexec/wupstreamd/wup-kms-helper ]; then
        setcap cap_sys_admin=ep /usr/local/libexec/wupstreamd/wup-kms-helper \
            || echo "warning: setcap failed on wup-kms-helper" >&2
    fi
    install -d -m 0755 /etc/systemd/system/wup-thinserver.service.d
    printf '%s\n' 'Environment=WUP_SESSION=kms' \
        > /etc/systemd/system/wup-thinserver.service.d/30-kms.conf
    python3 - /home/wup/.config/wup-thinserver/settings.json <<'PY' || true
import json, sys
p = sys.argv[1]
try:
    cfg = json.load(open(p))
except Exception:
    sys.exit(0)
cfg["capture_backend"] = 3
json.dump(cfg, open(p, "w"), indent=2)
PY
    chown wup:wup /home/wup/.config/wup-thinserver/settings.json 2>/dev/null || true
    systemctl enable wup-xorg.service
fi

# Pre-provisioned WiFi networks (headless setup): iwd reads known-network
# profiles from /var/lib/iwd/<SSID>.{psk,open,8021x}; networkd already runs
# DHCP on wlan0, iwd only performs association. Profiles are staged by
# build-rpi-image.sh under /opt/wup/networks/.
if [ -d /opt/wup/networks ]; then
    install -d -m 0700 -o root -g root /var/lib/iwd
    for profile in /opt/wup/networks/*.psk /opt/wup/networks/*.open /opt/wup/networks/*.8021x; do
        [ -f "$profile" ] || continue
        install -m 0600 -o root -g root "$profile" "/var/lib/iwd/$(basename "$profile")"
    done
fi

# No physical desktop or display manager. The single native service owns its
# Xvfb/XFCE/PulseAudio session. Console and SSH administration remain usable.
systemctl mask display-manager.service lightdm.service 2>/dev/null || true

# Headless first-boot configuration: wup-firstboot reads
# /boot/firmware/wup-firstboot.conf (the FAT32 BOOT partition, editable from
# any PC after flashing — the rpi-image-gen equivalent of Raspberry Pi
# Imager OS customization) and applies Wi-Fi credentials, passwords and
# hostname before networking comes up.
install -m 0755 /opt/wup/firstboot/wup-firstboot /usr/local/sbin/wup-firstboot
install -m 0644 /opt/wup/firstboot/wup-firstboot.service \
    /etc/systemd/system/wup-firstboot.service
install -m 0644 /opt/wup/firstboot/wup-firstboot.conf.example \
    /boot/firmware/wup-firstboot.conf.example
systemctl enable wup-firstboot.service

# First-boot rootfs expansion: the image ships a small root partition
# (root_part_size in the layer config) and wup-growfs grows it to fill the
# card on first boot, like Raspberry Pi OS / cloud-init do. Debian
# rpi-image-gen images have no such mechanism of their own. The pios
# (cloud-init) images already grow the filesystem themselves.
install -m 0755 /opt/wup/firstboot/wup-growfs /usr/local/sbin/wup-growfs
install -m 0644 /opt/wup/firstboot/wup-growfs.service \
    /etc/systemd/system/wup-growfs.service
systemctl enable wup-growfs.service

if [ "$PROFILE" = zero2w ]; then
    # A real reduced-memory profile: no compositing and no optional discovery,
    # printing or package-maintenance daemons in the 512 MiB appliance.
    install -d -m 0755 /home/wup/.config/xfce4/xfconf/xfce-perchannel-xml
    cat > /home/wup/.config/xfce4/xfconf/xfce-perchannel-xml/xfwm4.xml <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<channel name="xfwm4" version="1.0">
  <property name="general" type="empty">
    <property name="use_compositing" type="bool" value="false"/>
  </property>
</channel>
EOF
    chown -R wup:wup /home/wup/.config/xfce4
    for unit in apt-daily.timer apt-daily-upgrade.timer avahi-daemon.service \
                cups.service ModemManager.service triggerhappy.service; do
        systemctl mask "$unit" 2>/dev/null || true
    done
fi

touch /var/lib/wup-thinserver-rpi-image

