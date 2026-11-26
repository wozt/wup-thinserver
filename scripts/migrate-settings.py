#!/usr/bin/env python3
"""wup-migrate-settings: versioned, idempotent migration of the persisted
ThinServer settings.json inside the Docker home volume.

Runs as the `wup` user on every container start, before the daemon. It only
touches the input bindings introduced by the desktop-usability milestone;
personal files (documents, Firefox profile, downloads, other XFCE/dconf
settings) are never modified. Any file it edits is first copied to
<name>.pre-desktop-migration.bak (once).

Migrations applied:
  * input.stick_keys == legacy gaming defaults ["a","d","w","s",
    "Left","Right","Up","Down"] -> all empty (sticks now drive
    pointer/scroll).
  * input.right_stick_mouse -> 0 and right_stick_scroll -> 1
    (right stick scrolls instead of moving the pointer).
  * input.left_stick_mouse -> 1 (left stick moves the pointer).
  * input.mouse_buttons[L3/R3] -> 1/3 when unset (click like ZL/ZR).

A file already matching the new defaults is left untouched, so the script
is safe to run on every boot.
"""
import json
import os
import shutil
import sys

CFG = os.environ.get(
    "WUP_THIN_CONFIG",
    os.path.expanduser("~/.config/wup-thinserver/settings.json"),
)
OLD_STICK_KEYS = ["a", "d", "w", "s", "Left", "Right", "Up", "Down"]


def main() -> int:
    if not os.path.isfile(CFG):
        return 0
    try:
        with open(CFG, "r", encoding="utf-8") as f:
            cfg = json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        print("wup-migrate-settings: cannot parse %s: %s" % (CFG, e),
              file=sys.stderr)
        return 0  # daemon will fall back to defaults / report the error

    inp = cfg.get("input")
    if not isinstance(inp, dict):
        return 0

    changed = False

    sk = inp.get("stick_keys")
    if isinstance(sk, list) and sk[:8] == OLD_STICK_KEYS:
        inp["stick_keys"] = [""] * 8
        changed = True

    if inp.get("right_stick_mouse"):
        inp["right_stick_mouse"] = 0
        changed = True
    if "right_stick_scroll" not in inp:
        inp["right_stick_scroll"] = 1
        changed = True
    if "left_stick_mouse" not in inp:
        inp["left_stick_mouse"] = 1
        changed = True

    mb = inp.get("mouse_buttons")
    if isinstance(mb, list) and len(mb) == 18:
        # L3 (index 15) -> left click, R3 (16) -> right click, only when free.
        if not mb[15]:
            mb[15] = 1
            changed = True
        if not mb[16]:
            mb[16] = 3
            changed = True
    elif "mouse_buttons" not in inp:
        mb = [0] * 18
        mb[8], mb[9], mb[15], mb[16] = 1, 3, 1, 3
        inp["mouse_buttons"] = mb
        changed = True

    if not changed:
        return 0

    bak = CFG + ".pre-desktop-migration.bak"
    if not os.path.exists(bak):
        shutil.copy2(CFG, bak)
    tmp = CFG + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=2)
        f.write("\n")
    os.replace(tmp, CFG)
    print("wup-migrate-settings: updated %s (backup: %s)" % (CFG, bak))
    return 0


if __name__ == "__main__":
    sys.exit(main())
