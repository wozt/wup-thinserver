#!/bin/bash
# WUP ThinServer session entrypoint.
#
# Two phases:
#   root   -- account provisioning (password, sudo), then drop privileges
#   wup    -- the actual session: private D-Bus, Xvfb, PulseAudio, XFCE,
#             Onboard, wup-thinserver daemon.
#
# Account provisioning (root phase, idempotent, runs on every start):
#   WUP_PASSWORD        plaintext password for `wup` (env only; prefer
#                       WUP_PASSWORD_HASH or `wup-set-password`). Never
#                       logged. If neither is set the account keeps its
#                       locked password ('!' in /etc/shadow): console
#                       login and sudo stay disabled.
#   WUP_PASSWORD_HASH   a crypt(3) hash ('openssl passwd -6 ...') applied
#                       with usermod -p.
#   WUP_SUDO=1          add `wup` to the sudo group.
#   WUP_SUDO_NOPASSWD=1  passwordless sudo inside the container only
#                       (documented convenience; the container is the
#                       isolation boundary, not the host).
# Runtime password change: `docker exec -u root wup-thinserver wup-set-password`
# (interactive) — see docs/INSTALLATION.md.
set -u

if [ "$(id -u)" = "0" ]; then
    # --- root prologue: account provisioning --------------------------------
    if [ -n "${WUP_PASSWORD_HASH:-}" ]; then
        usermod -p "$WUP_PASSWORD_HASH" wup
        echo "[wup-thin] wup password set from WUP_PASSWORD_HASH"
    elif [ -n "${WUP_PASSWORD:-}" ]; then
        printf 'wup:%s\n' "$WUP_PASSWORD" | chpasswd
        echo "[wup-thin] wup password set from WUP_PASSWORD"
        unset WUP_PASSWORD
    else
        echo "[wup-thin] no WUP_PASSWORD(_HASH): 'wup' account stays locked"
    fi
    if [ "${WUP_SUDO:-0}" = "1" ]; then
        usermod -aG sudo wup
        echo "[wup-thin] wup added to the sudo group"
    fi
    if [ "${WUP_SUDO_NOPASSWD:-0}" = "1" ]; then
        printf 'wup ALL=(ALL) NOPASSWD:ALL\n' > /etc/sudoers.d/90-wup-nopasswd
        chmod 440 /etc/sudoers.d/90-wup-nopasswd
        echo "[wup-thin] passwordless sudo enabled inside the container"
    fi
    exec setpriv --reuid=wup --regid=wup --init-groups -- \
        /usr/local/bin/wup-thin-entrypoint --session "$@"
fi

[ "${1:-}" = "--session" ] || { echo "wup-thin-entrypoint: must run as root or --session as wup" >&2; exit 1; }
shift

DISPLAY_NUM="${DISPLAY_NUM:-99}"
export DISPLAY=":${DISPLAY_NUM}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/xdg-runtime}"
export PULSE_RUNTIME_PATH="${PULSE_RUNTIME_PATH:-/tmp/pulse-runtime}"
export DESKTOP_WIDTH="${DESKTOP_WIDTH:-1280}"
export DESKTOP_HEIGHT="${DESKTOP_HEIGHT:-720}"
export HOME=/home/wup

log() { echo "[wup-thin] $*"; }

# --- 0. private session D-Bus ------------------------------------------------
if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
    eval "$(dbus-launch --sh-syntax)"
    export DBUS_SESSION_BUS_ADDRESS DBUS_SESSION_BUS_PID
    log "session bus: $DBUS_SESSION_BUS_ADDRESS"
fi

PIDS=""
cleanup() {
    log "shutting down session"
    # Ask XFCE to log out politely first, then kill what remains.
    if [ -n "$PIDS" ]; then
        kill -TERM $PIDS 2>/dev/null
        sleep 2
        kill -KILL $PIDS 2>/dev/null
    fi
    exit 0
}
trap cleanup TERM INT

# --- 0b. persistent-config migrations (idempotent, backs up what it edits) ---
if [ -x /usr/local/bin/wup-migrate-settings ]; then
    /usr/local/bin/wup-migrate-settings || log "settings migration reported issues"
fi

# --- 1. Xvfb -----------------------------------------------------------------
Xvfb "$DISPLAY" -screen 0 "${DESKTOP_WIDTH}x${DESKTOP_HEIGHT}x24" \
    -nolisten tcp >/tmp/xvfb.log 2>&1 &
PIDS="$PIDS $!"

# Wait for the display to accept connections.
for i in $(seq 1 50); do
    xdpyinfo -display "$DISPLAY" >/dev/null 2>&1 && break
    sleep 0.1
done
if ! xdpyinfo -display "$DISPLAY" >/dev/null 2>&1; then
    log "ERROR: Xvfb did not come up; see /tmp/xvfb.log"
    exit 1
fi
log "Xvfb up on $DISPLAY (${DESKTOP_WIDTH}x${DESKTOP_HEIGHT})"

# Keyboard layout for the session (us, fr, ...).
if [ -n "${KEYBOARD_LAYOUT:-}" ]; then
    setxkbmap -layout "$KEYBOARD_LAYOUT" ${KEYBOARD_VARIANT:+-variant "$KEYBOARD_VARIANT"} \
        >/dev/null 2>&1 || log "setxkbmap failed for $KEYBOARD_LAYOUT"
fi

# --- 2. PulseAudio ----------------------------------------------------------
pulseaudio --start --exit-idle-time=-1 \
    --log-target=stderr >/tmp/pulseaudio.log 2>&1
# Give the daemon a real sink+monitor even with no audio hardware.
pactl load-module module-null-sink sink_name=thinserver \
    sink_properties=device.description=ThinServer >/dev/null 2>&1 || true
pactl set-default-sink thinserver >/dev/null 2>&1 || true
log "PulseAudio up (null sink 'thinserver')"

# --- 3. XFCE session ---------------------------------------------------------
# xfce4-session brings up the panel, window manager and desktop.
xfce4-session >/tmp/xfce4-session.log 2>&1 &
PIDS="$PIDS $!"

# Wait until the session is reasonably up (panel WM running).
for i in $(seq 1 100); do
    pgrep -x xfwm4 >/dev/null 2>&1 && pgrep -x xfce4-panel >/dev/null 2>&1 && break
    sleep 0.2
done

# Desktop readability tweaks for the 854x480 GamePad view.
xfconf-query -c xfce4-desktop -p /desktop-icons/style 2>/dev/null && \
    xfconf-query -c xfce4-desktop -p /desktop-icons/style -s 1 || true
xfconf-query -c xfwm4 -p /general/button_layout -s "O|SHMC" 2>/dev/null || true

# Install the settings launcher for new and existing persistent home volumes.
# The helper resolves the localized XDG desktop directory and never replaces
# a launcher the user already customized.
if [ -x /usr/local/bin/wup-install-desktop-shortcut ]; then
    /usr/local/bin/wup-install-desktop-shortcut || log "desktop shortcut installation reported issues"
fi

# --- 4. Onboard (on-screen keyboard) ----------------------------------------
# Apply the desktop keyboard defaults once per home volume (marker file),
# honouring ONBOARD_* env overrides every start.
if [ -x /usr/local/bin/onboard-setup ]; then
    /usr/local/bin/onboard-setup || log "onboard-setup reported issues"
fi
# Start it hidden; auto-show on text fields and the GamePad X toggle
# (onboard-toggle, a real D-Bus visibility toggle) bring it back.
onboard >/tmp/onboard.log 2>&1 &
PIDS="$PIDS $!"
sleep 2
gdbus call --session --dest org.onboard.Onboard \
    --object-path /org/onboard/Onboard/Keyboard \
    --method org.onboard.Onboard.Keyboard.Hide >/dev/null 2>&1 || true
log "Onboard started (auto-show + X toggle)"

# --- 5. wup-thinserver -------------------------------------------------------
if [ ! -f "$WUP_THIN_CONFIG" ]; then
    mkdir -p "$(dirname "$WUP_THIN_CONFIG")"
    cp /usr/local/share/wup-thinserver/settings.example.json "$WUP_THIN_CONFIG"
fi
wup-thinserver --config "$WUP_THIN_CONFIG" &
DAEMON_PID=$!
PIDS="$PIDS $DAEMON_PID"
log "wup-thinserver started (config $WUP_THIN_CONFIG)"

# The streaming daemon is the supervised process: desktop helpers may exit
# and be restarted independently, but if the daemon exits the session ends.
wait "$DAEMON_PID" 2>/dev/null
cleanup
