# Project status

Updated: 2026-11-26 (settings layout correction + direct connections)

Status legend: **planned** / **in progress** / **implemented** /
**tested automatically** / **validated on physical hardware** / **known issue**

## Milestone 0 — Audit & initialization

- [x] Both repositories initialized, remotes configured.
- [x] WUP Stream source audited; upstream commits recorded (`docs/UPSTREAM.md`).
- [x] AGENTS.md in both repos (git-ignored).
- [x] Initial documentation set.
- [x] Build system: Meson/Ninja for the daemon.

## Milestone 1 — Independent Linux desktop (Docker)

- [x] Dockerfile: Debian 13 + XFCE + Xvfb + Firefox ESR + Onboard + PulseAudio + non-root `wup` user + persistent `/home/wup`.
- [x] Entrypoint: dbus + Xvfb + pulseaudio + xfce4-session + daemon startup, SIGTERM handling, healthcheck.
- [x] docker-compose.yml (host networking, documented).
- [x] **Verified (automated smoke):** `docker compose up` → container healthy;
      xfwm4/xfce4-panel/xfdesktop/Thunar/Onboard/wup-thinserver all running
      on Xvfb :99; daemon in "looking for GamePad" state. Firefox ESR is
      installed and launchable (not yet opened in-session).
- [x] **Physical GamePad validation (developer-reported):** the XFCE desktop
      is visible and usable from WUP ThinClient on real hardware. This is the
      first end-to-end confirmation of the streaming pipeline.
- [ ] Onboard auto-appearance in text fields (implemented + D-Bus verified
      in container; pending physical retest).

## Milestone 2 — Headless streaming server

- [x] Daemon `wup-thinserver` builds on host (C11, Meson): vendored WUP Stream
      engine (capture, scaling, x264 encode, PulseAudio, XTest input, session loop).
- [x] JSON config + validation (`--check`), CLI, instance lock, syslog, signals.
- [x] Reconnect: `auto_reconnect` keeps the daemon waiting after a drop;
      the desktop session is unaffected (separate process).
- [x] Input mapping commands (`commands[]`) and mouse buttons (`mouse_buttons[]`).
- [ ] **Verified in container:** capture+encode of the Xvfb display.
- [ ] Protocol compatibility check against a real/pad client.

## Milestone 3 — WUP ThinClient

- [x] Dedicated homebrew app `wup_thinclient.wup` in wup-thinclient repo
      (server IP config + persistence, local menu); builds via libwup.
- [x] Automated host-side tests for config format / app.cfg.

## Milestones 4–8

planned (desktop interaction polish, session reliability hardening,
performance measurement, native install, Raspberry Pi).

## Live configuration interface

- [x] **Implemented / automatically tested:** same-user Unix socket with
      `0700` directory, `0600` socket, UID peer check, 64 KiB message bound,
      and no TCP listener.
- [x] **Implemented / automatically tested:** get, validate, apply, restore,
      daemon state, and current controller-state operations.
- [x] **Implemented / automatically tested:** invalid keys and new arbitrary
      commands are rejected without changing the valid file.
- [x] **Implemented / automatically tested:** atomic save precedes publication
      of a complete configuration snapshot to the streaming thread.
- [x] **Implemented:** old-map key chords, modifiers, stick keys, mouse
      buttons and touch drags are released before live replacement.

## Integrated WUP ThinServer Settings

- [x] **Implemented:** optional GTK3 executable; daemon-only builds use
      `-Dsettings_app=disabled` and do not require GTK.
- [x] **Implemented:** complete button/action editor, key capture and common
      keyboard-free actions, independent stick modes/directions, touch, gyro,
      microphone/volume options, live input diagnostics, reload and defaults.
- [x] **Implemented:** compact borderless WOZT-style layout, custom title bar,
      eight resize zones, framed sections, taskbar identity, icon/menu entry,
      and idempotent localized-XDG desktop shortcut installation.
- [x] **Automatically tested:** native build and IPC integration; Xvfb render
      at 854x480 has no GTK warnings and exposes `_NET_WM_ICON` and WM_CLASS.
- [x] **Automatically tested in Docker:** rebuilt image is healthy; existing
      persistent HOME received the executable/trusted launcher; app launched
      in the real XFCE session; live apply and restore preserved the active
      `streaming` connection.
- [ ] **Planned:** named profiles/import-export, detailed gesture thresholds,
      Onboard preferences, response curves, seven live JSON themes/languages,
      and a graphical controller silhouette.

## PC client, ARM64 and appliances

- [ ] **Planned:** WUP ThinClient PC feasibility, automated laboratory,
      native installer, QEMU ARM64, Raspberry Pi 4 and Zero 2 W images.
- **Known limitation:** GamePad raw-CABAC reconstruction for FFmpeg has not
  yet been demonstrated. QEMU results will not be treated as physical Pi
  performance or VideoCore compatibility evidence.

## Milestone 2b — Desktop usability (this milestone)

- [x] Desktop-oriented default bindings: left stick = pointer, right stick =
      scroll, ZL/ZR/L3/R3 = mouse buttons, X = `onboard-toggle` command,
      HOME reserved for the client menu. `stick_keys` empty by default.
- [x] Input reliability: command-bound buttons produce no key/mouse state,
      mouse buttons tracked in `mb_down` and released on disconnect,
      chords released only when actually pressed (`chord_down`),
      fractional scroll accumulation, zombie-free command spawning
      (no `G_SPAWN_DO_NOT_REAP_CHILD`), touch coordinates clamped.
- [x] Onboard: auto-show enabled + bottom docking via `onboard-setup`
      (gsettings, once per home volume); visibility-aware toggle via the
      real D-Bus API (`org.onboard.Onboard.Keyboard` `Visible`/`Show`/`Hide`)
      — verified in-container: repeated toggles alternate Visible
      true/false with a single Onboard process.
- [x] Hostname `wup-thinserver` (compose `hostname`, `THIN_HOSTNAME` override).
- [x] `wup` account provisioning: locked password by default,
      `WUP_PASSWORD`/`WUP_PASSWORD_HASH`/`wup-set-password`, optional
      `WUP_SUDO`/`WUP_SUDO_NOPASSWD`; entrypoint drops root→wup via setpriv.
- [x] Idempotent config migration (`wup-migrate-settings`) preserving the
      persistent home volume — verified: existing settings.json migrated
      with `.pre-desktop-migration.bak` backup, no volume reset.
- [ ] Physical retest: Onboard auto-show on text fields, X toggling,
      new stick/mouse bindings, manual connect flow.

## Input usability and gaming support

- [x] **Implemented / automatically tested:** XFixes cursor capture and
      premultiplied-alpha composition after server-side scaling. Both XCB
      MIT-SHM and GetImage are tested with a non-zero hotspot at 1280×720 →
      854×480; a fake-GamePad integration test verifies that the cursor
      reaches the encoded WUP video payload; cursor visibility defaults on.
- [ ] **Pending physical validation:** pointer shape, hotspot and movement in
      the actual encoded GamePad output.
- [x] **Implemented / automatically tested on the host:** live Desktop and
      Gamepad modes. The uinput device has stable identity, four centered
      axes, all 18 controls with Nintendo-position face-button mapping, and is
      detected through both evdev and SDL. Applying a mode releases held
      inputs and centers axes without restarting the stream.
- [x] **Implemented / automatically tested:** Settings and the protected IPC
      expose the configured mode, effective mode, verified availability, and
      useful uinput failure status. A failed Gamepad preflight leaves Desktop
      mode and the last valid settings intact.
- [x] **Implemented:** Docker gamepad overlay (`docker-compose.gamepad.yml`)
      — bind-mounted `/dev/uinput`, privately `mknod`'d `/dev/uinput-wup`,
      `wup-gamepad-devd` evdev-node watcher, `libsdl2` in the image, no
      privileged mode, no host input devices. Documented limitation: the
      virtual controller is visible to host-kernel SDL/evdev apps too.
- [x] **Implemented / automatically tested:** `'T'` live mode-switch protocol
      (request/ack + `'H'` mode byte) shared with the client; HOME is
      bindable server-side; `set_pad_brightness` IPC op with validation and
      persistence.
- [x] **Implemented:** single-page Settings dashboard (no tabs), larger
      font, work-area sizing, hardware brightness slider.
- [ ] **Pending Docker in-container validation and physical retest:** SDL/
      evdev detection inside the gamepad container; live HOME+X switching
      and HOME shortcuts on the real client.

## Settings layout correction and direct connections

- [x] **Implemented / screenshot-verified in-container:** single-page
      dashboard reorganized into vertical rows — compact input-mode strip,
      full-width bindings (two nine-button groups share the row via a
      GtkFlowBox size-allocate handler), three horizontally wrapped
      secondary sections, compact live-monitor strip, fixed footer.
      Directional-key rows only appear for keyboard-direction stick modes.
      Window = `_NET_WORKAREA` at 1280x720 / 1066x600 / 854x480; taskbar
      preserved; font unchanged. Fixed a GTK callback-signature crash on
      config load.
- [x] **Implemented / automatically tested:** `listen_port` (1–65535,
      default 50001) and `listen_address` (numeric IPv4 or empty)
      settings with validation tests; libwupstream patch
      `extend_direct_connections` adds `wupstream_open_port()` and records
      the beacon's real source port (kept updated from session packets).
- [x] **Verified live in Docker:** unicast `B` to `host:50001` and
      `host:5080` (custom `listen_port`) both produced `H` replies sent to
      the sender's arbitrary source port — the mechanism direct/WAN
      clients rely on (cone NAT, no inbound forwarding).
- [x] **Implemented / automatically tested:** `'T'` request dedup by
      consecutive reqid (recorded ack replayed, reset per session) and
      peer-port updates restricted to validated session packets
      (`B`/`N`/`M`/`T`/`I` type+length); `B` post-session is the defined
      NAT-rebinding path.
- [ ] **Pending:** isolated network-namespace NAT validation
      (translated ports, endpoint filtering, multi-client, mapping
      expiry); secure WAN transport feasibility; PC reference client;
      physical GamePad retest of the profile manager.

## Known issues / risks

- Core streaming validated on physical GamePad (developer-reported);
  usability fixes above pending physical retest.
- Host-networking requirement for pad discovery (documented).
- 60 fps not measured; software x264 at 720p may not sustain it.
- `commands[]`/`mouse_buttons[]`/`cmd_presets` are local additions (not upstream).
