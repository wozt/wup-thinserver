# Build

## Host build (native)

Requirements (Debian 13 shown):

```sh
sudo apt install meson ninja-build pkg-config \
  libglib2.0-dev libx11-dev libxrandr-dev libxtst-dev libxext-dev libxfixes-dev \
  libxinerama-dev libxcb1-dev libxcb-shm0-dev libx11-xcb-dev \
  libpulse-dev libcairo2-dev libpango1.0-dev libjson-glib-dev libgtk-3-dev \
  libswscale-dev libdrm-dev libgbm-dev libegl-dev libgles-dev \
  python3 git make
```

Build:

```sh
meson setup build
ninja -C build
./build/wup-thinserver --check
./build/wup-thinserver-settings
```

GTK3 is auto-detected. A daemon-only build has no GTK dependency:

```sh
meson setup build-daemon -Dsettings_app=disabled
meson compile -C build-daemon
```

The first build compiles a pinned, patched x264 inside `build/` via
`tools/build_wupstream_dep.py` (clones `videolan/x264` once, applies
`x264-drh-mode.patch` from the pinned wup-stream revision). It needs network
access on the first build only.

## Docker build

```sh
docker compose build          # or: docker build -f docker/Dockerfile .
```

The image builds the daemon and integrated settings application inside Debian
13 with the same Meson flow and installs them plus XFCE, Xvfb, Firefox ESR,
Onboard, Thunar, Mousepad, XFCE Terminal, PulseAudio and fonts.

## Client build

The WUP ThinClient homebrew is built in the `wup-thinclient` repository with
the libwup toolchain; see its `docs/BUILD.md`.

## ARM64 cross-build

`scripts/build-arm64.sh` cross-compiles the daemon, settings app and the
patched libwupstream/x264 stack for aarch64 using
`packaging/cross/aarch64-linux-gnu.ini` and the `aarch64-linux-gnu-*`
toolchain (gcc-aarch64-linux-gnu plus the `:arm64` -dev libraries listed
in `provisioning/packages/build-arm64.txt`).

```sh
./scripts/build-arm64.sh           # -> build-arm64/wup-thinserver(+ -settings)
qemu-aarch64-static /lib/ld-linux-aarch64.so.1 \
    ./build-arm64/wup-thinserver --help
```

Output binaries are plain `ELF aarch64` executables; verify with
`file build-arm64/wup-thinserver`. The QEMU image builder
(`scripts/build-qemu-image.sh`) reuses this cross-build to produce the
appliance payload.
