# Project status

Updated: 2026-11-26 (first development session)

Status legend: **planned** / **in progress** / **implemented** /
**tested automatically** / **validated on physical hardware** / **known issue**

## Milestone 0 — Audit & initialization

- [x] Both repositories initialized, remotes configured.
- [x] WUP Stream source audited; upstream commits recorded (`docs/UPSTREAM.md`).
- [x] AGENTS.md in both repos (git-ignored).
- [x] Initial documentation set.
- [x] Build system: Meson/Ninja for the daemon.

## Milestone 1 — Independent Linux desktop (Docker)

- [ ] Dockerfile: Debian 13 + XFCE + Xvfb + Firefox ESR + Onboard + PulseAudio + non-root `wup` user + persistent `/home/wup`.
- [ ] Entrypoint: dbus + Xvfb + pulseaudio + xfce4-session + daemon startup, SIGTERM handling, healthcheck.
- [ ] docker-compose.yml (host networking, documented).
- [ ] **Verified:** desktop runs without host X11/audio; Firefox & Onboard launch; display inspectable.

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

- [ ] Dedicated homebrew app (libwup) with own branding/menu/server selection.
- [ ] Artifact builds (`.wup`).
- [ ] Automated host-side tests for client logic.

## Milestones 4–8

planned (desktop interaction polish, session reliability hardening,
performance measurement, native install, Raspberry Pi).

## Known issues / risks

- Nothing validated on physical GamePad hardware yet.
- Host-networking requirement for pad discovery (documented).
- 60 fps not measured; software x264 at 720p may not sustain it.
- `commands[]`/`mouse_buttons[]` are local additions (not upstream).
- Onboard D-Bus toggle (`onboard-toggle`) untested against real Onboard.
