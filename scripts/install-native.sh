#!/usr/bin/env bash
# install-native.sh — idempotent native installer for WUP ThinServer.
#
# Supports Debian 13 on amd64 and arm64. Installs the daemon, the GTK
# Settings application, the streamed-desktop session (Xvfb + XFCE +
# PulseAudio null sink + Onboard + Firefox ESR), a dedicated unprivileged
# `wup` user, and a systemd unit that brings the whole appliance up at
# boot with no physical display or login screen.
#
# The streamed desktop never runs on the host's own display and never
# touches the host's graphical session.
#
# Usage:
#   sudo scripts/install-native.sh [options]
#   sudo scripts/install-native.sh --uninstall
#
# Options:
#   --desktop WxH       virtual display resolution (default 1280x720)
#   --keyboard LAYOUT   X11 keyboard layout (default us)
#   --port N            UDP listen port for the daemon (default: keep config)
#   --hostname NAME     set the system hostname (explicit opt-in only)
#   --password          prompt for the `wup` account password (never set a
#                       default; without it the account stays locked)
#   --prebuilt DIR      install from a staged usr/local tree instead of
#                       compiling (used by image builders)
#   --no-start          install everything but do not enable/start the unit
#   --uninstall         remove the installation (keeps /home/wup by default)
#   --remove-user       with --uninstall: also delete the wup account
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PKGDIR="$ROOT/provisioning/packages"
UNIT_SRC="$ROOT/packaging/systemd/wup-thinserver.service"
UNIT_DST=/etc/systemd/system/wup-thinserver.service
DROPIN_DIR=/etc/systemd/system/wup-thinserver.service.d
DROPIN="$DROPIN_DIR/10-wup.conf"
WUP_USER=wup
WUP_HOME=/home/$WUP_USER
WUP_CONFIG_DIR="$WUP_HOME/.config/wup-thinserver"
WUP_CONFIG="$WUP_CONFIG_DIR/settings.json"

DESKTOP="1280x720"; KEYBOARD="us"; PORT=""; HOSTNAME=""; PASSWORD=0
PREBUILT=""; NO_START=0; UNINSTALL=0; REMOVE_USER=0
while [ $# -gt 0 ]; do case "$1" in
    --desktop) DESKTOP="$2"; shift 2;;
    --keyboard) KEYBOARD="$2"; shift 2;;
    --port) PORT="$2"; shift 2;;
    --hostname) HOSTNAME="$2"; shift 2;;
    --password) PASSWORD=1; shift;;
    --prebuilt) PREBUILT="$2"; shift 2;;
    --no-start) NO_START=1; shift;;
    --uninstall) UNINSTALL=1; shift;;
    --remove-user) REMOVE_USER=1; shift;;
    -h|--help) sed -n '2,30p' "$0"; exit 0;;
    *) echo "unknown option $1" >&2; exit 2;;
esac; done

log() { printf '\033[1;36m[install-native]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[install-native] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "must run as root"
[ -r /etc/os-release ] || die "not a systemd/Debian system"
. /etc/os-release
[ "${ID:-}" = "debian" ] || log "warning: not Debian ($ID) — package lists may differ"
ARCH="$(dpkg --print-architecture 2>/dev/null || uname -m)"
case "$ARCH" in amd64|arm64) ;; *) die "unsupported architecture: $ARCH";; esac

pkgs() { grep -vE '^\s*(#|$)' "$1"; }

# ---------- uninstall ---------------------------------------------------------
if [ "$UNINSTALL" = 1 ]; then
    systemctl disable --now wup-thinserver.service 2>/dev/null || true
    rm -f "$UNIT_DST"; rm -rf "$DROPIN_DIR"
    systemctl daemon-reload 2>/dev/null || true
    if [ -d "$ROOT/build" ] && command -v meson >/dev/null; then
        meson uninstall -C "$ROOT/build" 2>/dev/null || true
    else
        for f in wup-thinserver wup-thinserver-settings wup-thin-entrypoint \
                 onboard-toggle onboard-setup wup-install-desktop-shortcut \
                 wup-migrate-settings wup-set-password wup-gamepad-devd; do
            rm -f "/usr/local/bin/$f"
        done
        rm -rf /usr/local/share/wup-thinserver
        rm -f /usr/local/share/applications/wup-thinserver-settings.desktop \
              /usr/local/share/icons/hicolor/scalable/apps/wup-thinserver-settings.svg
    fi
    if [ "$REMOVE_USER" = 1 ]; then
        userdel -r "$WUP_USER" 2>/dev/null || die "could not remove $WUP_USER"
        log "removed user $WUP_USER"
    else
        log "kept user $WUP_USER and $WUP_HOME"
    fi
    log "uninstalled (package removal is left to the administrator)"
    exit 0
fi

# ---------- packages ------------------------------------------------------------
log "installing packages (arch: $ARCH)"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq $(pkgs "$PKGDIR/runtime.txt") $(pkgs "$PKGDIR/desktop.txt")
if [ -z "$PREBUILT" ]; then
    apt-get install -y -qq $(pkgs "$PKGDIR/build.txt")
fi
command -v meson >/dev/null || [ -n "$PREBUILT" ] || die "meson missing after package install"

# ---------- account ---------------------------------------------------------------
if ! id "$WUP_USER" >/dev/null 2>&1; then
    useradd -m -s /bin/bash "$WUP_USER"
    log "created user $WUP_USER ($WUP_HOME)"
else
    log "user $WUP_USER already exists"
fi
mkdir -p "$WUP_CONFIG_DIR"; chown -R "$WUP_USER:$WUP_USER" "$WUP_CONFIG_DIR"

if [ "$PASSWORD" = 1 ]; then
    read -rsp "Password for $WUP_USER: " PW1; echo
    read -rsp "Confirm: " PW2; echo
    [ "$PW1" = "$PW2" ] || die "passwords differ"
    [ -n "$PW1" ] || die "empty password refused"
    printf '%s:%s\n' "$WUP_USER" "$PW1" | chpasswd
    unset PW1 PW2
    log "password set for $WUP_USER"
fi

# ---------- build / install ---------------------------------------------------------
if [ -n "$PREBUILT" ]; then
    [ -d "$PREBUILT/usr/local" ] || die "prebuilt dir lacks usr/local: $PREBUILT"
    cp -a "$PREBUILT/usr/local/." /usr/local/
    log "installed prebuilt tree from $PREBUILT"
else
    cd "$ROOT"
    if [ ! -d build ] || [ "${WUP_REBUILD:-0}" = 1 ]; then
        rm -rf build
        meson setup build --buildtype=release -Dsettings_app=enabled
    fi
    meson compile -C build -j"$(nproc)"
    meson install -C build
    log "built and installed to /usr/local"
fi

# ---------- session state dirs -------------------------------------------------------
mkdir -p /tmp/pulse-runtime /tmp/xdg-runtime
chmod 700 /tmp/pulse-runtime /tmp/xdg-runtime
chown "$WUP_USER:$WUP_USER" /tmp/pulse-runtime /tmp/xdg-runtime

# ---------- daemon settings -----------------------------------------------------------
if [ ! -f "$WUP_CONFIG" ]; then
    cp /usr/local/share/wup-thinserver/settings.example.json "$WUP_CONFIG"
    chown "$WUP_USER:$WUP_USER" "$WUP_CONFIG"
fi
if [ -n "$PORT" ]; then
    python3 - "$WUP_CONFIG" "$PORT" <<'PY'
import json, sys
path, port = sys.argv[1], int(sys.argv[2])
cfg = json.load(open(path))
cfg["listen_port"] = port
json.dump(cfg, open(path, "w"), indent=2)
PY
    chown "$WUP_USER:$WUP_USER" "$WUP_CONFIG"
    log "settings updated (listen_port=$PORT)"
fi

# ---------- systemd ---------------------------------------------------------------------
install -m 644 "$UNIT_SRC" "$UNIT_DST"
mkdir -p "$DROPIN_DIR"
{
    echo "# managed by scripts/install-native.sh — do not edit"
    echo "[Service]"
    echo "Environment=DESKTOP_WIDTH=${DESKTOP%x*}"
    echo "Environment=DESKTOP_HEIGHT=${DESKTOP#*x}"
    echo "Environment=KEYBOARD_LAYOUT=${KEYBOARD}"
    echo "Environment=WUP_THIN_CONFIG=${WUP_CONFIG}"
} > "$DROPIN"
chmod 644 "$DROPIN"
systemctl daemon-reload
if [ "$NO_START" = 0 ]; then
    systemctl enable --now wup-thinserver.service
    log "wup-thinserver.service enabled and started"
else
    systemctl enable wup-thinserver.service
    log "wup-thinserver.service enabled (not started: --no-start)"
fi

# ---------- hostname (explicit opt-in only) ----------------------------------------------
if [ -n "$HOSTNAME" ]; then
    hostnamectl set-hostname "$HOSTNAME" 2>/dev/null || hostname "$HOSTNAME"
    log "hostname set to $HOSTNAME"
fi

log "done. The streamed desktop starts at boot on display :99 (virtual)."
log "Admin: systemctl status wup-thinserver.service ; journalctl -u wup-thinserver -f"
