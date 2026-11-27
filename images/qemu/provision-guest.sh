#!/bin/bash
# provision-guest.sh — first-boot provisioning inside the QEMU ARM64 guest.
# Runs as root from cloud-init runcmd. Idempotent: package installs and
# unit enablement are safe to re-run; user/home are preserved.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
WUP=/opt/wup
LOG(){ echo "[provision-guest] $*"; }

LOG "installing runtime packages"
dpkg --configure -a 2>/dev/null || true   # recover from an interrupted run
apt-get update -qq
pkgs() { grep -vE '^\s*(#|$)' "$1"; }
apt-get install -y -qq $(pkgs "$WUP/packages/runtime.txt")
LOG "installing desktop packages"
apt-get install -y -qq $(pkgs "$WUP/packages/desktop.txt")

# ThinServer binaries + helpers were unpacked to / by the payload tarball.
[ -x /usr/local/bin/wup-thinserver ] || { echo "missing /usr/local/bin/wup-thinserver"; exit 1; }

# Dedicated unprivileged session user (locked password: SSH/admin uses keys).
if ! id wup >/dev/null 2>&1; then
    useradd -m -s /bin/bash wup
    LOG "created user wup"
fi
# Baseline appliance accounts: wup/wup in sudo, root/wup.
printf 'wup:wup\nroot:wup\n' | chpasswd
usermod -aG sudo wup
mkdir -p /home/wup/.config/wup-thinserver /tmp/pulse-runtime /tmp/xdg-runtime
chown -R wup:wup /home/wup /tmp/pulse-runtime /tmp/xdg-runtime
chmod 700 /tmp/pulse-runtime /tmp/xdg-runtime

# Daemon configuration (defaults; Settings app can change them later).
if [ ! -f /home/wup/.config/wup-thinserver/settings.json ]; then
    cp /usr/local/share/wup-thinserver/settings.example.json \
       /home/wup/.config/wup-thinserver/settings.json
fi
chown wup:wup /home/wup/.config/wup-thinserver/settings.json

# systemd drop-in: virtual display + session environment.
mkdir -p /etc/systemd/system/wup-thinserver.service.d
cat > /etc/systemd/system/wup-thinserver.service.d/10-wup.conf <<'EOF'
[Service]
Environment=HOME=/home/wup
Environment=DESKTOP_WIDTH=1280
Environment=DESKTOP_HEIGHT=720
Environment=DISPLAY_NUM=99
Environment=KEYBOARD_LAYOUT=us
Environment=WUP_THIN_CONFIG=/home/wup/.config/wup-thinserver/settings.json
EOF
chmod 644 /etc/systemd/system/wup-thinserver.service.d/10-wup.conf

systemctl daemon-reload
systemctl enable wup-thinserver.service
LOG "wup-thinserver.service enabled"

# Mark provisioning done (kept for diagnostics).
touch /var/lib/wup-thinserver-provisioned
LOG "done"
