#!/bin/sh
# onboard-toggle: show or hide the Onboard on-screen keyboard.
#
# Invoked by wup-thinserver when the GamePad's X button is pressed
# (input.commands[WBTN_X] in settings.json). Runs inside the session's
# D-Bus/X11 environment, so DISPLAY and DBUS_SESSION_BUS_ADDRESS must be
# set by the caller (the daemon inherits them from the session launcher).
#
# Onboard exposes org.onboard.Onboard on the session bus. When the method
# calls are unavailable we fall back to launching/hiding via wmctrl-free
# heuristics: a running onboard is hidden through its D-Bus Hide method;
# otherwise a fresh `onboard` is started.

set -eu

call() {
    # call METHOD; returns 0 when the bus call succeeded
    if command -v gdbus >/dev/null 2>&1; then
        gdbus call --session --dest org.onboard.Onboard \
            --object-path /Onboard \
            --method "org.onboard.Onboard.$1" >/dev/null 2>&1
    elif command -v dbus-send >/dev/null 2>&1; then
        dbus-send --session --print-reply \
            --dest=org.onboard.Onboard /Onboard \
            "org.onboard.Onboard.$1" >/dev/null 2>&1
    else
        return 1
    fi
}

running() {
    pgrep -x onboard >/dev/null 2>&1
}

if running; then
    if call Hide; then
        exit 0
    fi
    # D-Bus method missing: fall back to SIGTERM (onboard saves its state).
    pkill -x onboard 2>/dev/null || true
    exit 0
fi

# Not running: try the D-Bus Show first (covers "hidden but alive" states),
# then start the keyboard docked at the bottom of the screen.
if call Show; then
    exit 0
fi
exec onboard --layout "Compact" >/dev/null 2>&1 &
exit 0
