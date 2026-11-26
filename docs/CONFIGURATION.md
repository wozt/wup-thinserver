# Configuration

The daemon reads one JSON file. Resolution order:

1. `--config PATH` on the command line (sets `WUP_THIN_CONFIG`),
2. `WUP_THIN_CONFIG` environment variable,
3. `~/.config/wup-thinserver/settings.json`.

Validate without starting: `wup-thinserver --check`.
An annotated example ships at `config/settings.example.json` (installed to
`/usr/share/wup-thinserver/settings.example.json` or the image's
`/etc/wup-thinserver/`).

## Top-level keys

| Key | Type | Default | Meaning |
|---|---|---|---|
| `pad_ip` | string | `""` | GamePad IP. Empty = accept the first pad announcing itself. Set it to pin one client. |
| `source_type` | int | `0` | `0` screen (whole display), `1` window, `2` region. ThinServer uses `0`. |
| `aspect` | int | `1` | `0` stretch, `1` letterbox, `2` crop — how the desktop maps into 854x480. |
| `capture_backend` | int | `0` | `0` XCB MIT-SHM, `1` XCB GetImage, `2` KMS CPU, `3` KMS EGL. Xvfb → `0` or `1`. |
| `preset_name` / `crf` / `kbps` / `custom_quality` | | `Balanced`, 18, 10000 | x264 quality. Presets: Economy (23/4000), Balanced (18/10000), Max (14/20000). |
| `low_latency` | int | `1` | zerolatency x264: no B-frames/lookahead. Keep `1` for the GamePad decoder. |
| `audio_pc_to_pad` | int | `1` | Stream session audio to the GamePad. |
| `audio_pad_to_pc` | int | `0` | Play the GamePad microphone on the session. |
| `pad_volume_controls_pc` | int | `1` | Physical volume slider → PulseAudio sink volume (hysteresis-filtered). |
| `audio_monitor` | string | `"default"` | PulseAudio monitor source; `"default"` = default sink's monitor. |
| `autostart_stream` | int | `1` | Begin streaming as soon as a client connects. |
| `auto_reconnect` | int | `1` | Keep waiting for the client after a disconnect (session persists). |
| `gamepad_only` | int | `0` | Test mode: static frame, no capture. |

## `source`

`output`: XRandR output name; empty = first screen (the Xvfb root).
`x`,`y`,`w`,`h`: used by region sources only.

## `input`

| Key | Meaning |
|---|---|
| `mode` | `0` = virtual evdev gamepad (`/dev/uinput`), `1` = XTest keyboard/mouse (**default**, required for Onboard typing) |
| `deadzone` | Stick deadzone in raw units (default 120, max ~±1100) |
| `sensitivity` | Pointer/stick speed percent |
| `invert_y` | Invert stick Y |
| `gyro_mode` / `gyro_sens` / `gyro_bias` | Gyro→mouse/stick, sensitivity, calibrated bias |
| `touch_as_mouse` | Touchscreen → pointer gestures (tap/drag/scroll/right-click) |
| `left_stick_mouse` | Left stick → relative pointer motion (desktop default `1`) |
| `right_stick_mouse` | Right stick → relative pointer motion (legacy; `0` by default) |
| `right_stick_scroll` | Right stick → wheel scroll, horizontal + vertical (desktop default `1`) |
| `keys[18]` | X11 keysym chords per button, order: `A B X Y ← → ↑ ↓ ZL ZR L R + − HOME L3 R3 TV`. Chords like `"Ctrl+Page_Up"` supported. |
| `stick_keys[8]` | Keys for stick directions: `LX- LX+ LY- LY+ RX- RX+ RY- RY+` (empty by default — sticks drive pointer/scroll instead) |
| `commands[18]` | Shell command run on each **press** edge (kb/mouse mode). E.g. `["", "", "onboard-toggle", ...]` toggles Onboard on X. |
| `mouse_buttons[18]` | X11 button (1=left, 2=middle, 3=right, 4/5=wheel) on press/release edges. Takes precedence over `keys`. |
| `cmd_presets` / `cmd_preset` | Named keymaps saved for reuse |

### Default GamePad mapping (example config)

| GamePad | Action |
|---|---|
| Touch | absolute pointer + gestures (tap / double-tap / long-press right-click / drag / scroll) |
| Left stick | relative pointer motion (`left_stick_mouse`) |
| Right stick | wheel scroll, horizontal + vertical (`right_stick_scroll`) |
| ZL / L3 | left mouse button (`mouse_buttons`) |
| ZR / R3 | right mouse button (`mouse_buttons`) |
| A / B | Enter / Escape |
| X | toggle Onboard (`commands[WBTN_X]` = `onboard-toggle`) |
| Y | Alt+Tab |
| L / R | previous / next workspace (Ctrl+Page_Up / Ctrl+Page_Down) |
| D-pad | arrow keys |
| + / − | zoom (Ctrl+plus / Ctrl+minus) |
| TV | fullscreen (F11) |
| HOME | local ThinClient menu — never forwarded to the desktop |
| Volume slider | PulseAudio volume |

`commands[]` owns a button entirely: a press edge runs the shell command and
no key or mouse action is produced for that button (so a command-bound button
cannot also emit a stray keysym). `mouse_buttons[]` maps a button to an X11
mouse button (1 left, 2 middle, 3 right, 4/5 wheel) on press/release edges
and takes precedence over `keys[]` when both are set. Held mouse buttons are
released on client disconnect.

All of it is configuration — nothing is hard-coded.

## Environment overrides (libwupstream)

`WUPSTREAM_CRF`, `WUPSTREAM_KBPS`, `WUPSTREAM_X264` (`key=value:...`),
`WUPSTREAM_DEBUG=1`, `WUPSTREAM_DROP=n` (loss test), `WUPSTREAM_DUMP=file`,
`WUPSTREAM_PHASE_MS`, `WUP_CAPTURE_ASYNC=0`, `WUPSTREAMD_DEBUG=1`.

## Desktop/session settings (launcher, not the daemon)

`DESKTOP_WIDTH` (854|1066|1280|1920 → default 1280), `DESKTOP_HEIGHT`
(480|600|720|1080 → default 720), `DESKTOP_DEPTH` (24), `KEYBOARD_LAYOUT`
(`us`, `fr`, ...), `KEYBOARD_VARIANT`, `LOCALE`.
