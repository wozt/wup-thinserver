# Upstream source audit & reuse

WUP ThinServer reuses the WUP Stream engine. This file records exactly what
was taken, from where, and what was changed locally.

## Upstream repositories

| Repo | URL | Commit used | Role |
|---|---|---|---|
| wup-stream-server-linux | https://github.com/wozt/wup-stream-server-linux | `254539d260fc840738bb3401bcb1032a06df7a8a` (2026-11-26) | server engine source (`src/`) |
| wup-stream | https://github.com/wozt/wup-stream | `e1b04305ca123e3f6980d60ef302ada9c19ce127` (2026-11-22) | `libwupstream` (protocol + x264) and pad receiver reference |
| wup-homebrew (fork) | https://github.com/ravensclawx/wup-homebrew | local `wup-homebrew-with-lan-storage` @ `7b641902` (branch `feature/netfs`) | libwup toolchain for the client |

`tools/build_wupstream_dep.py` pins `wozt/wup-stream` at `e1b0430` and builds
`libwupstream.a` (wupstream.c + patched x264) inside the Meson build dir.

## Imported files (server)

Copied verbatim from `wup-stream-server-linux/src/` unless noted:

- `settings.c/h` — **modified**: config dir → `~/.config/wup-thinserver`,
  defaults for daemon (`autostart_stream=1`, `auto_reconnect=1`,
  `input.mode=INMODE_KBMOUSE`), `WUP_THIN_CONFIG` path override, new
  `input.commands[]` (shell command per button press) and
  `input.mouse_buttons[]` (X11 button per press/release).
- `server.c/h` — unmodified session/stream loop.
- `capture.c/h`, `capture_async.c/h`, `capture_kms.c/h`, `kms_wire.h` —
  unmodified X11/KMS capture + scaling.
- `audio.c/h` — unmodified PulseAudio capture/playback.
- `input.c/h` — **modified**: `kb_buttons()` honours `commands[]` (press
  edge, `/bin/sh -c`, detached) and `mouse_buttons[]` (XTest button edges)
  before falling back to keysym chords.
- `x11util.c/h` — unmodified XRandR/EWMH/XTest helpers.
- `i18n.c/h` — **modified**: GTK UI helpers (`wup_i18n_fill_combo`,
  `wup_i18n_translate_tree`) removed for the headless daemon; catalog and
  `wup_i18n_lookup` kept (used by the gamepad-only test frame). Paths
  renamed to `wup-thinserver`.

## Not imported (GTK/UI-only, intentionally dropped)

`ui.c/h`, `viz.c/h`, `select.c/h`, `window_pick.c/h`, `kms_permissions.c/h`,
`kms_helper.c`, `main.c` (GTK). Replaced by `src/main.c` (headless CLI),
`src/config.c/h` (validation).

## Protocol / encoder findings (audit summary)

- UDP only. Pad listens :50000; discovery broadcast :50001 (`B` beacons).
- `H` hello (250 ms), `V` video fragments ≤1400 B of raw CABAC slices,
  `A` 48 kHz s16 stereo PCM, `R` rumble, `Q` quit; `I` 128-byte input
  report ~200 Hz, `N` NACKs, `M` mic.
- x264: pinned commit `b35605ace3ddf7c1a5d67a2eb553f034aef41d55` +
  `x264-drh-mode.patch` (no slice headers / planar prediction — required by
  the pad's decoder ring). High profile, CABAC, 854×480, ~60 fps,
  CRF + VBV cap (default 10 Mbps).
- Frame pacing: one frame per pad refresh (59.94 Hz), measured from `I`
  clock ticks; cushion of 2 frames + resend ring of 8; NACK-driven resend.
- Touch coordinates are calibrated pad-side to 0..853 × 0..479; server maps
  them onto the streamed root rect (`input_set_touch_map`).
- Stability fixes preserved: disconnect releases all inputs
  (`input_release_held`), capture restart without dropping the pad session,
  volume-slider hysteresis, decoder watchdog/keyframe recovery (client),
  async capture pipeline.

## Limitations

- `pad_ip` empty = first-come discovery; set it to pin a single client.
- No authentication/encryption (trusted LAN only).
- KMS capture backends compile but are unvalidated in this project.
- `commands[]`/`mouse_buttons[]` are local additions, not upstream features.
