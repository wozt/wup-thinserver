# Configuration

The daemon reads one JSON file. Routine input changes should use WUP
ThinServer Settings; direct editing remains available for advanced fields.
Resolution order:

1. `--config PATH` on the command line (sets `WUP_THIN_CONFIG`),
2. `WUP_THIN_CONFIG` environment variable,
3. `~/.config/wup-thinserver/settings.json`.

Validate without starting: `wup-thinserver --check`.
An annotated example ships at `config/settings.example.json` (installed in
the shared-data directory). Docker stores the active file in the persistent
home volume at `/home/wup/.config/wup-thinserver/settings.json`.

Successful settings writes use atomic same-directory replacement. Invalid IPC
updates and failed writes leave both the active configuration and the last
valid file unchanged.

## Top-level keys

| Key | Type | Default | Meaning |
|---|---|---|---|
| `pad_ip` | string | `""` | GamePad IP. Empty = accept the first pad announcing itself. Set it to pin one client. |
| `listen_port` | int | `50002` | UDP port the daemon binds for `B` beacons (LAN broadcast and direct unicast endpoints). 1–65535. ThinServer uses a distinct default from WUP Stream. |
| `listen_address` | string | `""` | Numeric IPv4 address to bind the beacon socket to; empty = all interfaces. Restrict it (e.g. the LAN address) to limit exposure. Hostnames are not accepted — resolve to an IPv4 literal. |
| `source_type` | int | `0` | `0` screen (whole display), `1` window, `2` region. ThinServer uses `0`. |
| `aspect` | int | `1` | `0` stretch, `1` letterbox, `2` crop — how the desktop maps into 854x480. |
| `capture_backend` | int | `0` | `0` XCB MIT-SHM, `1` XCB GetImage, `2` KMS CPU, `3` KMS EGL. Xvfb → `0` or `1`. |
| `cursor_visible` | int | `1` | Composite the current X11 cursor into the encoded frame. Applies to X11 and KMS CPU capture; KMS EGL does not expose a BGRA composition stage. |
| `preset_name` / `crf` / `kbps` / `custom_quality` | | `Balanced`, 18, 10000 | x264 quality. Presets: Economy (23/4000), Balanced (18/10000), Max (14/20000). |
| `low_latency` | int | `1` | zerolatency x264: no B-frames/lookahead. Keep `1` for the GamePad decoder. |
| `audio_pc_to_pad` | int | `1` | Stream session audio to the GamePad. |
| `audio_pad_to_pc` | int | `0` | Play the GamePad microphone on the session. |
| `pad_volume_controls_pc` | int | `1` | Physical volume slider → PulseAudio sink volume (hysteresis-filtered). |
| `pad_brightness` | int | `-1` | GamePad hardware backlight: `-1` leave the pad's level alone, `1`–`6` discrete levels. Applied live to a connected pad, persisted, and adjustable from Settings. `-1` also means "not managed" — the pad keeps its own setting. |
| `camera_announce` | int | `0` | Request the GamePad camera and publish decoded 640×480 YUYV frames to the dedicated V4L2 output. Opt-in and independent from preview. |
| `camera_preview` | int | `0` | Request the GamePad camera and publish the latest decoded frame to the protected Settings preview. Opt-in and independent from Announce. |
| `camera_device` | string | `"/dev/video42"` | Numbered V4L2 output node. ThinServer accepts it only when its card label is exactly `Wii U GamePad Camera`; physical/unrelated webcams are rejected. |
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
| `mode` | `0` = virtual evdev Gamepad (`/dev/uinput`), `1` = XTest Desktop keyboard/mouse (**default**, required for Onboard typing). Settings applies this live. |
| `deadzone` | Stick deadzone in raw units (default 120, max ~±1100) |
| `sensitivity` | Pointer/stick speed, 0–50 (default 25); legacy values >50 clamp to 50 |
| `invert_y` | Invert stick Y |
| `gyro_mode` / `gyro_sens` / `gyro_bias` | Gyro→mouse/stick, sensitivity, calibrated bias |
| `touch_as_mouse` | Touchscreen → pointer gestures (tap/drag/scroll/right-click) |
| `left_stick_mouse` | Left stick → relative pointer motion (desktop default `1`) |
| `right_stick_mouse` | Right stick → relative pointer motion (legacy; `0` by default) |
| `right_stick_scroll` | Right stick → wheel scroll, horizontal + vertical (desktop default `1`) |
| `keys[18]` | X11 keysym chords per button, order: `A B X Y ← → ↑ ↓ ZL ZR L R + − HOME L3 R3 TV`. Chords like `"Ctrl+Page_Up"` supported. HOME is bindable; the ThinClient reserves only its local shortcut combinations (5 s hold, HOME+X/Up/Down/B), never the plain tap. |
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
| HOME | short tap forwards the configured binding (default `Super_L`, the XFCE app menu); ThinClient keeps 5 s hold → local menu, HOME+X → live mode switch, HOME+Up/Down → local backlight, HOME+B → exit. Bindable like any other button in `keys[]`/`commands[]`/`mouse_buttons[]`. |
| Volume slider | PulseAudio volume |

`commands[]` owns a button entirely: a press edge runs the shell command and
no key or mouse action is produced for that button (so a command-bound button
cannot also emit a stray keysym). `mouse_buttons[]` maps a button to an X11
mouse button (1 left, 2 middle, 3 right, 4/5 wheel) on press/release edges
and takes precedence over `keys[]` when both are set. Held mouse buttons are
released on client disconnect.

The graphical application exposes only the safe `onboard-toggle` built-in;
it cannot create arbitrary shell commands. Existing advanced commands remain
available through deliberate JSON editing on this trusted local session.

All of it is configuration — nothing is hard-coded.

### Virtual controller mapping

Gamepad mode creates `WUP ThinServer Gamepad` with four signed evdev axes
(`ABS_X`, `ABS_Y`, `ABS_RX`, `ABS_RY`, range −32767…32767) and the following
buttons. The face buttons follow their physical Nintendo positions:

| GamePad control | Linux evdev code |
|---|---|
| A / B / X / Y | `BTN_EAST` / `BTN_SOUTH` / `BTN_NORTH` / `BTN_WEST` |
| L / R | `BTN_TL` / `BTN_TR` |
| ZL / ZR | `BTN_TL2` / `BTN_TR2` |
| L3 / R3 | `BTN_THUMBL` / `BTN_THUMBR` |
| Plus / Minus / HOME | `BTN_START` / `BTN_SELECT` / `BTN_MODE` |
| TV | `BTN_TRIGGER_HAPPY1` |
| D-pad | `BTN_DPAD_LEFT`, `BTN_DPAD_RIGHT`, `BTN_DPAD_UP`, `BTN_DPAD_DOWN` |

Desktop key, command, mouse-button, and stick bindings are suppressed while
Gamepad mode is effective. Touch-as-mouse and gyro-as-mouse are separately
configurable and can remain enabled. When uinput creation fails, ThinServer
reports the reason and keeps Desktop mode effective instead of leaving input
partially active.

The effective mode can also be switched **live** while streaming: the
ThinClient sends a `'T'` control packet (HOME+X) or the Settings app uses
the IPC `apply_input`. The daemon validates Gamepad availability, applies
the switch without dropping the stream, persists `input.mode`, and — for
`'T'` requests — acknowledges with the effective mode. A Gamepad request
that cannot be satisfied is rejected and Desktop stays active.

## Environment overrides (libwupstream)

`WUPSTREAM_CRF`, `WUPSTREAM_KBPS`, `WUPSTREAM_X264` (`key=value:...`),
`WUPSTREAM_DEBUG=1`, `WUPSTREAM_DROP=n` (loss test), `WUPSTREAM_DUMP=file`,
`WUPSTREAM_PHASE_MS`, `WUP_CAPTURE_ASYNC=0`, `WUPSTREAMD_DEBUG=1`.

## GamePad camera

Camera capture is disabled by default. Announce and Preview may be enabled
separately; when both are enabled they share one H.264 decoder and capture
request. Preview uses one latest-frame mmap slot under the same-UID runtime
directory, not JSON or base64, and the Settings process stops reading it when
its camera section or window is not visible. Disabling both consumers stops
the client camera request, flushes the decoder, invalidates the preview, and
writes one black frame before closing an active virtual webcam.

For Docker, use `docker-compose.camera.yml`; for native installs, create a
dedicated v4l2loopback device with the exact label documented in
[INSTALLATION.md](INSTALLATION.md). The network stream remains trusted-LAN
only; enabling camera does not add authentication or encryption.

## Desktop/session settings (launcher, not the daemon)

`DESKTOP_WIDTH` (854|1066|1280|1920 → default 1280), `DESKTOP_HEIGHT`
(480|600|720|1080 → default 720), `DESKTOP_DEPTH` (24), `KEYBOARD_LAYOUT`
(`us`, `fr`, ...), `KEYBOARD_VARIANT`, `LOCALE`.
