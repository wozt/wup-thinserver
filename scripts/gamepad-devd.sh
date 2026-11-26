#!/bin/bash
# wup-gamepad-devd: expose the virtual gamepad's evdev node inside the
# container. Runs as root, detached, started by the entrypoint only when
# the gamepad compose overlay provides /dev/uinput.
#
# The kernel creates /sys/class/input/eventN for "WUP ThinServer Gamepad"
# but there is no udev inside the container, so this script mknods the
# matching /dev/input/eventN (major 13) and hands it to the `wup` user.
# It also removes nodes whose sysfs device disappeared (mode switches
# destroy and recreate the uinput device).
set -u

LOG=/tmp/gamepad-devd.log
DEV_DIR=/dev/input

node_for_event() {  # $1 = eventN -> prints /dev/input/eventN when live
    local ev="$1" sys="/sys/class/input/$ev"
    [ -d "$sys/device" ] || return 1
    [ "$(cat "$sys/device/name" 2>/dev/null)" = "WUP ThinServer Gamepad" ] || return 1
    printf '%s/%s\n' "$DEV_DIR" "$ev"
}

mkdir -p "$DEV_DIR"
chmod 755 "$DEV_DIR"

while :; do
    # create nodes for live virtual devices
    for sysname in /sys/class/input/event*/device/name; do
        [ -e "$sysname" ] || continue
        ev="$(basename "$(dirname "$(dirname "$sysname")")")"
        node="$(node_for_event "$ev")" || continue
        [ -e "$node" ] && continue
        majmin="$(cat "/sys/class/input/$ev/dev" 2>/dev/null)" || continue
        major="${majmin%%:*}"; minor="${majmin##*:}"
        if mknod "$node" c "$major" "$minor" 2>/dev/null; then
            chown wup:wup "$node" 2>/dev/null
            chmod 0660 "$node" 2>/dev/null
            echo "[gamepad-devd] created $node ($majmin)" >>"$LOG"
        fi
    done
    # drop nodes whose device vanished (mode switches recreate the pad)
    for node in "$DEV_DIR"/event*; do
        [ -e "$node" ] || continue
        ev="$(basename "$node")"
        [ -d "/sys/class/input/$ev/device" ] || { rm -f "$node"; echo "[gamepad-devd] removed stale $node" >>"$LOG"; }
    done
    sleep 0.2
done
