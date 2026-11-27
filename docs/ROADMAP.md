# Roadmap

Status terms: **planned**, **in progress**, **implemented**,
**automatically tested**, **validated on physical hardware**, and
**known limitation** are intentionally distinct.

1. **Baseline — implemented / automatically tested.** Audit independent
   server and GamePad-client builds, Docker health, protocol pins, and NetFS
   deployment. Physical usability retest remains open.
2. **Live configuration — implemented / automatically tested.** Protected
   local IPC, validation, atomic persistence, input-state release, and live
   snapshot publication.
3. **Integrated settings application — in progress.** The real GTK editor,
   live monitor, icon, menu entry, and persistent-home desktop migration are
   implemented. Host-side Desktop/Gamepad switching, evdev mapping and SDL
   detection are automatically tested; Docker device integration and the
   HOME+X client shortcut remain in progress. Profiles, detailed gesture
   controls, seven JSON themes, and JSON localization remain planned. Docker
   runtime validation passes for Desktop mode.
4. **PC protocol client — planned.** Independent C11/Meson repository,
   discovery/handshake, exact packet reassembly, and GamePad-targeted H.264
   reconstruction feasibility before SDL presentation claims.
5. **SDL client and automated laboratory — planned.** Video/audio, viewport
   touch mapping, Xbox controller, headless scenarios, screenshots, measured
   diagnostics, and deterministic fault injection.
6. **Native Debian installation — done (scripted).** `provisioning/`
   shares package lists and the session entrypoint between Docker and
   native; `scripts/install-native.sh` installs the daemon, settings app,
   XFCE/Onboard stack and `wup-thinserver.service` for the unprivileged
   `wup` user without touching the host desktop.
7. **QEMU ARM64 — in progress.** `scripts/build-qemu-image.sh` produces
   `build/images/wup-thinserver-qemu-arm64.qcow2` (Debian 13 genericcloud
   arm64 + shared native provisioning via cloud-init); `run-qemu.sh`
   launches headless profiles func/pi4/zero2w with UDP hostfwd 5080→50001
   and SSH admin forward 2222→22. Emulation validates function only,
   never physical Raspberry Pi performance.
8. **Raspberry Pi 4 — planned.** Reproducible headless ARM64 image and physical
   validation; patched software x264 remains the known-compatible start.
9. **Raspberry Pi Zero 2 W — planned / experimental.** 512 MiB profile and
   physical measurement before any FPS or browser-performance claim.
10. **Performance and hardware encoding — planned.** Measure first; enable no
    hardware encoder until proven compatible with the physical GamePad.
