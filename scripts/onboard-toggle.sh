#!/bin/sh
# onboard-toggle: show or hide the Onboard on-screen keyboard.
#
# Invoked by wup-thinserver when the GamePad's X button is pressed
# (input.commands[WBTN_X] in settings.json). Runs inside the session's
# D-Bus/X11 environment (DISPLAY and DBUS_SESSION_BUS_ADDRESS are
# inherited from the session launcher).
#
# Onboard 1.4.x (Debian 13) exposes:
#   service  org.onboard.Onboard
#   path     /org/onboard/Onboard/Keyboard
#   iface    org.onboard.Onboard.Keyboard
#   methods  Show() Hide() ToggleVisible()
#   props    Visible (ro, bool), AutoShowPaused (rw, bool)
#
# Toggle = read Visible, then Show or Hide (explicit, no guessing).
# Fallbacks: ToggleVisible(), then Show() when nothing is running.

set -eu

BUS_DEST="org.onboard.Onboard"
BUS_PATH="/org/onboard/Onboard/Keyboard"
BUS_IFACE="org.onboard.Onboard.Keyboard"

visible() {
    # prints "true"/"false"; rc 1 when the property cannot be read
    gdbus call --session --dest "$BUS_DEST" --object-path "$BUS_PATH" \
        --method org.freedesktop.DBus.Properties.Get \
        "$BUS_IFACE" Visible 2>/dev/null | grep -o 'true\|false' | head -1
}

method() {
    gdbus call --session --dest "$BUS_DEST" --object-path "$BUS_PATH" \
        --method "$BUS_IFACE.$1" >/dev/null 2>&1
}

running() { pgrep -x onboard >/dev/null 2>&1; }

if ! command -v gdbus >/dev/null 2>&1; then
    # Last resort without gdbus: dbus-send for ToggleVisible.
    if running; then
        dbus-send --session --print-reply --dest="$BUS_DEST" "$BUS_PATH" \
            "$BUS_IFACE.ToggleVisible" >/dev/null 2>&1 || true
    else
        exec onboard >/dev/null 2>&1 &
    fi
    exit 0
fi

if running; then
    v="$(visible || true)"
    case "$v" in
        true)  method Hide ;;
        false) method Show ;;
        *)      method ToggleVisible || method Show ;;
    esac
    exit 0
fi

# Not running: start it, then make sure it is actually visible.
exec onboard >/dev/null 2>&1 &
