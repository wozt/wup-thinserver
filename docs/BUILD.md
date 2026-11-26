# Build

## Host build (native)

Requirements (Debian 13 shown):

```sh
sudo apt install meson ninja-build pkg-config \
  libglib2.0-dev libx11-dev libxrandr-dev libxtst-dev libxext-dev \
  libxinerama-dev libxcb1-dev libxcb-shm0-dev libx11-xcb-dev \
  libpulse-dev libcairo2-dev libpangocairo-1.0-0 libjson-glib-dev \
  libswscale-dev libdrm-dev libgbm-dev libegl-dev libgles-dev \
  python3 git make
```

Build:

```sh
meson setup build
ninja -C build
./build/wup-thinserver --check
```

The first build compiles a pinned, patched x264 inside `build/` via
`tools/build_wupstream_dep.py` (clones `videolan/x264` once, applies
`x264-drh-mode.patch` from the pinned wup-stream revision). It needs network
access on the first build only.

## Docker build

```sh
docker compose build          # or: docker build -f docker/Dockerfile .
```

The image builds the daemon inside Debian 13 with the same Meson flow and
installs it plus XFCE, Xvfb, Firefox ESR, Onboard, Thunar, Mousepad,
XFCE Terminal, PulseAudio and fonts.

## Client build

The WUP ThinClient homebrew is built in the `wup-thinclient` repository with
the libwup toolchain; see its `docs/BUILD.md`.
