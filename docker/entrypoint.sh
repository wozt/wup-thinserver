#!/bin/bash
# WUP ThinServer session entrypoint.
#
# Brings up, as the unprivileged `wup` user:
#   1. a private D-Bus session (dbus-launch)
#   2. Xvfb virtual display ($DESKTOP_WIDTH x $DESKTOP_HEIGHT)
#   3. PulseAudio with a null sink (the daemon captures its monitor)
#   4. XFCE session (panel, WM, desktop)
#   5. wup-thinserver daemon
#
# SIGTERM/SIGINT stop everything cleanly; the desktop state lives in
# /home/wup (persistent volume) — running apps do NOT survive a container
# restart, but files/settings do.
set -u

DISPLAY_NUM="${DISPLAY_NUM:-99}"
export DISPLAY=":${DISPLAY_NUM}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/xdg-runtime}"
export PULSE_RUNTIME_PATH="${PULSE_RUNTIME_PATH:-/tmp/pulse-runtime}"
export DESKTOP_WIDTH="${DESKTOP_WIDTH:-1280}"
export DESKTOP_HEIGHT="${DESKTOP_HEIGHT:-720}"

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

# --- 4. Onboard (on-screen keyboard) ----------------------------------------
# Start it hidden-ish; the GamePad X button toggles it via `onboard-toggle`.
onboard >/tmp/onboard.log 2>&1 &
PIDS="$PIDS $!"
sleep 2
# Hide it at startup; auto-show on text fields stays enabled.
gdbus call --session --dest org.onboard.Onboard --object-path /Onboard \
    --method org.onboard.Onboard.Hide >/dev/null 2>&1 || true
log "Onboard started (toggle with GamePad X)"

# --- 5. wup-thinserver -------------------------------------------------------
if [ ! -f "$WUP_THIN_CONFIG" ]; then
    mkdir -p "$(dirname "$WUP_THIN_CONFIG")"
    cp /etc/wup-thinserver/settings.json "$WUP_THIN_CONFIG" 2>/dev/null || true
fi
wup-thinserver --config "$WUP_THIN_CONFIG" &
DAEMON_PID=$!
PIDS="$PIDS $DAEMON_PID"
log "wup-thinserver started (config $WUP_THIN_CONFIG)"

# The streaming daemon is the supervised process: desktop helpers may exit
# and be restarted independently, but if the daemon exits the session ends.
wait "$DAEMON_PID" 2>/dev/null
cleanup
