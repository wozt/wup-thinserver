# Architecture

## Components

### wup-thinserver (this repository, C11, Meson/Ninja)

A headless daemon. Modules (in `src/`):

| Module | Role |
|---|---|
| `main.c` | CLI (`--config`, `--check`, `--version`), signals, instance lock, syslog |
| `config.c` / `settings.c` | JSON configuration load/save/validate (json-glib) |
| `server.c` | Session loop: pad discovery, capture, encode, audio, input dispatch, reconnect |
| `capture.c` / `capture_async.c` | X11 screen capture (XCB MIT-SHM or GetImage) + libswscale scaling to 854x480 BGRA |
| `capture_kms.c` | Optional DRM/KMS backends (2/3); unused on Xvfb |
| `audio.c` | PulseAudio monitor capture (PC→pad) and mic playback (pad→PC), 48 kHz s16 |
| `input.c` | XTest key/button/wheel injection + touch gestures; optional uinput virtual gamepad; per-button shell commands and mouse buttons |
| `x11util.c` | XRandR screens, EWMH windows, XTest helpers |
| `i18n.c` | UI string catalog (English compiled in; JSON languages optional) |
| `control.c` | Same-user Unix socket for validated live input updates and diagnostics |
| `libwupstream` | WUP Stream protocol + patched x264 encoder (pinned upstream, built by `tools/build_wupstream_dep.py`) |

The daemon never touches the host desktop: it opens whatever `DISPLAY`
points to. In the container that is the private Xvfb display.

### WUP ThinServer Settings (GTK3, optional Meson target)

`wup-thinserver-settings` is a separate process in the private desktop. It
does not link into the capture/encode daemon and never mutates streaming
memory. It exchanges bounded JSON messages over a protected Unix socket. The
daemon validates and atomically persists a candidate before publishing a
complete settings snapshot to its worker. See [CONTROL_IPC.md](CONTROL_IPC.md).

### Session launcher (container `docker/entrypoint.sh`, native `scripts/`)

Owns the desktop session lifecycle, independent of the daemon:

1. `dbus-run-session` provides a private session bus.
2. `Xvfb :99 -screen 0 <W>x<H>x24` creates the virtual display.
3. `pulseaudio` starts a private sound server (null sink + monitor).
4. `xfce4-session` starts the desktop (panel, window manager, desktop).
5. `wup-thinserver` starts once the display answers `xdpyinfo`.
6. An idempotent helper installs the settings launcher in the localized XDG
   desktop directory for new and existing persistent homes.

Because the daemon and the desktop are separate processes, the Linux session
survives GamePad disconnects and daemon restarts.

### wup-thinclient (separate repository)

A libwup homebrew app for the GamePad: WUP Stream receiver (hardware H.264
decoder ring, PCM audio, input report) plus a small local menu for server
selection. See its `docs/ARCHITECTURE.md`.

## Protocol

Unchanged WUP Stream protocol (UDP):

- pad → broadcast :50001 `B` announcement; PC discovers it.
- PC → pad :50000 `H` hello/keepalive, `V` video fragments (1400-byte
  payloads of raw CABAC slice data), `A` PCM audio, `R` rumble, `Q` quit.
- pad → PC `I` input report (128 bytes), `N` missing-packet NACKs,
  `M` microphone.

Full details in [PROTOCOL.md](PROTOCOL.md) (mirrors upstream `pad/stream`).

## Video path

X11 root window → MIT-SHM `GetImage` (or XCB fallback) → libswscale scale to
854x480 BGRA (letterbox/crop/stretch) → RGB→I420 in `wupstream_frame()` →
patched x264 (High profile, CABAC, no B-frames, zerolatency option) → slice
data fragmented into UDP packets paced to the pad's 59.94 Hz refresh.

Server-side scaling happens **before** encoding; the GamePad never allocates
a framebuffer for the desktop resolution.

## Input path

`wupstream_input()` parses the 128-byte report (buttons, sticks with per-pad
rest calibration, touch coordinates calibrated to 854x480, volume slider,
battery, IMU). `input_dispatch()` maps it into the session's X display via
XTest (kb/mouse mode, the default) or a `/dev/uinput` virtual gamepad.
On disconnect, `input_release_held()` releases every pressed key/button.

## Audio path

PulseAudio monitor of the default sink → 48 kHz stereo s16 ring → `A`
packets. Pad microphone → `M` packets → PulseAudio playback stream. The
physical volume slider drives `pactl set-sink-volume` with hysteresis.

## Security model (prototype)

- Trusted LAN only. No authentication, no encryption on the wire.
- The container is unprivileged, has no host X11/audio/docker socket access,
  and its own network namespace (bridge) — see `docker-compose.yml`.
- GamePad input is injected only into the session display (`DISPLAY`), never
  the host's.
