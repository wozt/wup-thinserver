#!/bin/sh
# onboard-setup: apply the ThinServer Onboard defaults.
#
# Runs inside the session (as wup, with DISPLAY + DBUS_SESSION_BUS_ADDRESS
# set) before Onboard starts. Product defaults are applied once per home
# volume (marker file); explicit ONBOARD_* environment overrides are
# applied on every start so they always win.
#
#   ONBOARD_DOCK_HEIGHT   docked keyboard height in px (default 190)
#   ONBOARD_LAYOUT        onboard layout name (e.g. a QWERTY/AZERTY variant)
set -eu

MARKER="$HOME/.config/wup-thinserver/.onboard-desktop-v1"
mkdir -p "$(dirname "$MARKER")"

gs() { gsettings set "$@" 2>/dev/null || true; }

# --- one-time desktop defaults ----------------------------------------------
if [ ! -f "$MARKER" ]; then
    # Auto-appearance on editable fields (AT-SPI) + bottom docking.
    gs org.onboard.auto-show enabled true
    gs org.onboard.window docking-enabled true
    gs org.onboard.window docking-edge bottom
    gs org.onboard.window.landscape dock-height "${ONBOARD_DOCK_HEIGHT:-190}"
    gs org.onboard.start-minimized false
    # Hint GTK apps to expose accessibility (harmless where unsupported).
    gs org.gnome.desktop.interface toolkit-accessibility true
    touch "$MARKER"
    echo "[onboard-setup] desktop defaults applied"
fi

# --- per-start overrides ------------------------------------------------------
if [ -n "${ONBOARD_DOCK_HEIGHT:-}" ]; then
    gs org.onboard.window.landscape dock-height "$ONBOARD_DOCK_HEIGHT"
fi
if [ -n "${ONBOARD_LAYOUT:-}" ]; then
    gs org.onboard layout "$ONBOARD_LAYOUT"
fi

exit 0
