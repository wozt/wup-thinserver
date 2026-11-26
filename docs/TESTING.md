# Testing

## Automated (host)

```sh
meson setup build && ninja -C build
./build/wup-thinserver --check          # config validation
./build/wup-thinserver --version
meson test -C build                     # unit tests (see tests/)
```

### What is covered

- `tests/test_config.py` — JSON config load/validate: defaults, invalid
  values rejected (`source_type`, `capture_backend`, `crf`, `kbps`,
  `input.mode`, `deadzone`, region geometry), `commands`/`mouse_buttons`
  round-trip through `settings_save`/`settings_load`.
- `tests/test_geometry.py` — touch→desktop coordinate transform and
  aspect geometry (letterbox/crop/stretch math extracted for the test).
- `tests/test_control.py` — isolated Xvfb daemon, socket permissions, live
  get/apply/restore, atomic persistence, and rejection of invalid keys and
  arbitrary IPC commands.

For a visual smoke test inside the container:

```sh
docker compose exec --user wup wup-thinserver-settings
docker compose exec --user wup sh -lc \
  'test -x "$(xdg-user-dir DESKTOP)/wup-thinserver-settings.desktop"'
```

### Not yet covered (planned)

- End-to-end stream against the real client (needs the GamePad or the
  `wup-emu` emulator from wup-homebrew).
- Docker healthcheck in CI.
- Reconnect/session-persistence test with a scripted fake client
  (a Python UDP peer speaking `H`/`I`/`N` is planned).

## Manual / hardware validation checklist

Requires a Wii U GamePad (firmware 7677) with WUP ThinClient installed,
on the same LAN as the server.

- [ ] Client connects; desktop appears within a few seconds.
- [ ] Firefox ESR launches from the XFCE menu; web pages render.
- [ ] Touch tap = left click; drag draws; long-press = right click.
- [ ] Left stick moves pointer; right stick scrolls.
- [ ] ZL/ZR = left/right click (hold works for drag-select).
- [ ] Onboard keyboard toggles with X; typing works in Firefox/Terminal.
- [ ] A=Enter, B=Escape, D-pad arrows, Y=Alt+Tab, L/R=browser tabs,
      +=zoom in, −=zoom out, TV=fullscreen.
- [ ] Volume slider changes session audio volume smoothly.
- [ ] Disconnect (HOME+B quit or Wi-Fi off): Linux apps keep running;
      reconnect restores the same desktop state.
- [ ] `docker compose restart` keeps `/home/wup` data.
- [ ] Measure: capture FPS, encoded FPS, CPU%, RAM, bitrate, latency
      (Milestone 6 — nothing is claimed until measured).

Never mark an item validated without running it on physical hardware.
