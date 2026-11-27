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

# No physical desktop or display manager. The single native service owns its
# Xvfb/XFCE/PulseAudio session. Console and SSH administration remain usable.
systemctl mask display-manager.service lightdm.service 2>/dev/null || true

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

