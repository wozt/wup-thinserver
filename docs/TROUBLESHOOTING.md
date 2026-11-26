# Troubleshooting

## Daemon won't start

- `DISPLAY is not set` — the daemon needs the session's X display. In the
  container the entrypoint sets `DISPLAY=:99`; natively, start your session
  launcher first.
- `another instance is already running` — remove the stale lock only if no
  daemon is running: `rm ~/.config/wup-thinserver/wup-thinserver.lock`.
- `invalid configuration` — run `wup-thinserver --check` for the exact
  field; see `docs/CONFIGURATION.md`.

## Client never connects

- GamePad and server must be on the same L2 network (broadcast discovery).
  With `network_mode: host` (default compose) the daemon sees LAN broadcasts.
- Set `pad_ip` in `settings.json` to the GamePad's IP to skip discovery.
- Check the daemon log: `state 1 - looking for GamePad...` means it is
  waiting; `no GamePad found, retrying` = beacons not arriving.
- Both WUP Stream server and ThinServer use UDP 50001/50000 — don't run
  both against the same pad.

## Black screen but connected

- XFCE may still be starting; wait ~10 s after container start.
- Check capture backend: on Xvfb use `0` (MIT-SHM) or `1` (GetImage).
- `gamepad_only: 1` produces a test pattern — if that shows, capture is
  the problem, not the network.
- Inspect the display directly (see INSTALLATION.md → "Inspecting the
  virtual display").

## No audio

- PulseAudio must be running inside the session (`pactl info` as user `wup`).
- `audio_monitor: "default"` maps to the default sink's monitor; the
  container creates a null sink for the session.
- Firefox/HTML5 audio needs the session's PulseAudio, not the host's.

## Input does nothing / wrong window

- `input.mode` must be `1` (kb/mouse XTest) for the desktop; `0` creates a
  `/dev/uinput` gamepad that XFCE won't treat as keyboard/mouse.
- All XTest events go to `DISPLAY` — verify it's the session display, not
  the host's (`:0` on the host desktop = wrong, dangerous).
- Touch maps onto the streamed root rect; a changed desktop resolution
  requires restarting the session (Xvfb size is fixed at start).

## Onboard doesn't toggle

- `onboard-toggle` must be on PATH inside the session (`/usr/local/bin` in
  the image). The daemon runs it with the session's `DBUS_SESSION_BUS_ADDRESS`.
- Check `pgrep -x onboard` inside the container; check `gdbus` availability.

## Performance

- Software x264 at 1280x720→854x480/60 fps is CPU-heavy; if the host
  struggles, lower `kbps`, raise `crf`, or use `DESKTOP_WIDTH=1066
  DESKTOP_HEIGHT=600`. 30 fps is not directly configurable yet — the
  encoder is paced by the pad's 59.94 Hz refresh (it repeats frames).
  VA-API is planned (Milestone 6), not implemented.
