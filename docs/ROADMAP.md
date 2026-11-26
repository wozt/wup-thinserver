# Roadmap

## Done / in this session

- Milestone 0: repos, audit, docs, AGENTS.md, build system.
- Milestone 1 (partial): Docker environment definition.
- Milestone 2 (partial): headless daemon builds and validates config.

## Next (priority order)

1. **Finish Milestone 1**: build the image, boot the desktop, verify
   XFCE/Firefox/Onboard launch, screenshot the Xvfb display, healthcheck.
2. **Milestone 2 validation**: run daemon in the container against a real
   WUP ThinClient (or the original WUP Stream pad app as a protocol check).
3. **Milestone 3**: WUP ThinClient homebrew — local menu, manual server IP,
   saved config, `.wup` artifact.
4. **Milestone 4**: input mapping end-to-end (Onboard toggle, browser
   tabs/zoom, volume), disconnect release verification.
5. **Milestone 5**: session persistence tests, bounded memory, diagnostics.
6. **Milestone 6**: measure FPS/CPU/bitrate/latency; then evaluate VA-API.
7. **Milestone 7**: native Debian installer + systemd units (share
   provisioning with Docker).
8. **Milestone 8**: Raspberry Pi 4 validation; Zero 2 W profile last.

## CI/CD (planned)

- GitHub Actions: meson compile + `--check` + tests + warning flags.
- Docker image build + smoke test (container boots, `xdpyinfo` answers).
- Client `.wup` cross-build in CI (libwup toolchain container).
- Releases: four-component versions `1.0.0.x`, GHCR images, checksums —
  only after end-to-end hardware validation.
