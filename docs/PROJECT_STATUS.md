# Project status

Updated: 2026-11-27 (desktop-usability milestone)

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

## Known issues / risks

- Core streaming validated on physical GamePad (developer-reported);
  usability fixes above pending physical retest.
- Host-networking requirement for pad discovery (documented).
- 60 fps not measured; software x264 at 720p may not sustain it.
- `commands[]`/`mouse_buttons[]`/`cmd_presets` are local additions (not upstream).
