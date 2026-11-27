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
  values rejected (`source_type`, `capture_backend`, `cursor_visible`, `crf`, `kbps`,
  `input.mode`, `deadzone`, region geometry), `commands`/`mouse_buttons`
  round-trip through `settings_save`/`settings_load`.
- `tests/test_geometry.py` — touch→desktop coordinate transform and
  aspect geometry (letterbox/crop/stretch math extracted for the test).
- `tests/test_control.py` — isolated Xvfb daemon, socket permissions, live
  get/apply/restore, atomic persistence, and rejection of invalid keys and
  arbitrary IPC commands; input-mode state, availability, preflight, and
  persistence are exercised without weakening socket isolation.
- `tests/capture_test.c` — real Xvfb capture through both MIT-SHM and XCB
  GetImage; defines a cursor with a non-zero hotspot and verifies its pixels,
  scaled position and size in the final 854×480 BGRA frame.
- `tests/test_encoded_cursor.py` — runs the real daemon and a minimal fake
  GamePad twice, reconstructs a complete WUP `V` frame, and verifies that
  enabling the cursor changes the encoded video payload.
- `tests/input_gamepad_test.c` — creates the real uinput controller, opens its
  evdev node, verifies SDL enumeration and stable identity, checks all 18
  Nintendo-position button mappings and a stick axis, then verifies held
  button release, axis centering, live Desktop/Gamepad transitions, and device
  cleanup. It skips with exit 77 when `/dev/uinput` is unavailable.
- `tests/test_control.py` covers `set_pad_brightness` (range validation,
  atomic persistence, rejection of bad levels) and live mode switching via
  `apply_input` (effective-mode reporting, Gamepad preflight rejection,
  persistence). The `'T'` pad→daemon packet path shares `input_set_mode`
  and is exercised end-to-end only with a real/fake GamePad session.

### Inside Docker (gamepad overlay)

With `docker-compose.gamepad.yml` the same checks run in-container:

```sh
docker compose -f docker-compose.yml -f docker-compose.gamepad.yml up -d --build
# build the host test binary, then copy it in (it is not shipped in the image):
cp build/wup-gamepad-test /tmp/
sudo docker cp /tmp/wup-gamepad-test wup-thinserver:/tmp/
sudo docker exec --user wup wup-thinserver /tmp/wup-gamepad-test
sudo docker exec wup-thinserver cat /sys/class/input/event*/device/name | grep -i wup
```

The watcher log is `/tmp/gamepad-devd.log` inside the container.

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

## Isolated NAT laboratory

`scripts/nat-lab.sh` (root required) builds `tools/nat-lab/lab-server`
against the pinned patched libwupstream and runs it inside a private
network namespace behind an nftables masquerade. `tools/nat-lab/
fake_pad.py` is a real-protocol UDP client (B/H/I/V/A/N/T) inside a
second namespace. The host network and firewall are never touched;
everything is torn down on exit (`--keep` retains the namespaces).

```sh
sudo scripts/nat-lab.sh                  # all tests
sudo scripts/nat-lab.sh --only session   # one test
sudo scripts/nat-lab.sh --only strict-filter
```

Tests: `endpoint` (H must come from the dialed port), `custom-port`
(same on UDP 5080), `session` (full B/H/I/V/A + T ack + N through NAT),
`strict-filter` (address-and-port-dependent filtering emulation —
replies must originate from the exact ip:port dialed), `isolation`
(client B's valid packets from another port must not interrupt client
A's stream; B connects after A leaves), `multi` (two clients sharing
one NAT external IPv4, turn-taking sessions), `regression` (a failing
fake client must fail the harness).

## Manual / hardware validation checklist

Requires a Wii U GamePad (firmware 7677) with WUP ThinClient installed,
on the same LAN as the server.

- [ ] Client connects; desktop appears within a few seconds.
- [ ] Firefox ESR launches from the XFCE menu; web pages render.
- [ ] Touch tap = left click; drag draws; long-press = right click.
- [ ] Left stick moves pointer; right stick scrolls.
- [ ] Pointer shape is visible at the correct position in the GamePad video.
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
