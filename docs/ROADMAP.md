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
   implemented. Profiles, detailed gesture controls, seven JSON themes, and
   JSON localization remain planned. Docker runtime validation passes.
4. **PC protocol client — planned.** Independent C11/Meson repository,
   discovery/handshake, exact packet reassembly, and GamePad-targeted H.264
   reconstruction feasibility before SDL presentation claims.
5. **SDL client and automated laboratory — planned.** Video/audio, viewport
   touch mapping, Xbox controller, headless scenarios, screenshots, measured
   diagnostics, and deterministic fault injection.
6. **Native Debian installation — planned.** Shared provisioning and systemd
   without modifying the host user's desktop.
7. **QEMU ARM64 — planned.** Generic `virt` image first; emulation validates
   function and memory pressure, never physical Raspberry Pi performance.
8. **Raspberry Pi 4 — planned.** Reproducible headless ARM64 image and physical
   validation; patched software x264 remains the known-compatible start.
9. **Raspberry Pi Zero 2 W — planned / experimental.** 512 MiB profile and
   physical measurement before any FPS or browser-performance claim.
10. **Performance and hardware encoding — planned.** Measure first; enable no
    hardware encoder until proven compatible with the physical GamePad.
