# WUP ThinServer — Debian 13 (trixie) desktop + streaming image.
#
# Multi-stage: builder compiles wup-thinserver with Meson; runtime keeps only
# the desktop session and the daemon binary.

ARG DEBIAN_IMAGE=debian:trixie-slim

# ---------------------------------------------------------------------------
# Stage 1: build the daemon
# ---------------------------------------------------------------------------
FROM ${DEBIAN_IMAGE} AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential meson ninja-build pkg-config python3 git make \
        ca-certificates \
        libglib2.0-dev libx11-dev libxrandr-dev libxtst-dev libxext-dev libxfixes-dev \
        libxinerama-dev libxcb1-dev libxcb-shm0-dev libx11-xcb-dev \
        libpulse-dev libcairo2-dev libpango1.0-dev libjson-glib-dev \
        libgtk-3-dev \
        libswscale-dev libdrm-dev libgbm-dev libegl-dev libgles-dev \
        nasm \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN meson setup build --buildtype=release -Dsettings_app=enabled \
    && ninja -C build \
    && DESTDIR=/out meson install -C build

# ---------------------------------------------------------------------------
# Stage 2: runtime — desktop session + daemon
# ---------------------------------------------------------------------------
FROM ${DEBIAN_IMAGE}

ARG DESKTOP_WIDTH=1280
ARG DESKTOP_HEIGHT=720
ARG DISPLAY_NUM=99

ENV DEBIAN_FRONTEND=noninteractive \
    DISPLAY=:${DISPLAY_NUM} \
    XDG_SESSION_TYPE=x11 \
    DESKTOP_WIDTH=${DESKTOP_WIDTH} \
    DESKTOP_HEIGHT=${DESKTOP_HEIGHT} \
    KEYBOARD_LAYOUT=us \
    KEYBOARD_VARIANT= \
    LOCALE=C.UTF-8 \
    WUP_THIN_CONFIG=/home/wup/.config/wup-thinserver/settings.json \
    PULSE_RUNTIME_PATH=/tmp/pulse-runtime \
    XDG_RUNTIME_DIR=/tmp/xdg-runtime

# Runtime libraries the daemon links against (built in stage 1).
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        libglib2.0-0 libx11-6 libxrandr2 libxtst6 libxext6 libxfixes3 libxinerama1 \
        libxcb1 libxcb-shm0 libx11-xcb1 \
        libpulse0 libcairo2 libpangocairo-1.0-0 libjson-glib-1.0-0 \
        libgtk-3-0 \
        libswscale8 libdrm2 libgbm1 libegl1 libgles2 \
        libpulse0 \
    && rm -rf /var/lib/apt/lists/*

# --- session: virtual display, audio, desktop -------------------------------
RUN apt-get update && apt-get install -y --no-install-recommends \
        xvfb x11-xserver-utils x11-utils x11-apps \
        dbus dbus-x11 dbus-user-session \
        pulseaudio pulseaudio-utils \
        xfce4-session xfwm4 xfce4-panel xfdesktop4 \
        thunar xfce4-terminal mousepad \
        firefox-esr \
        onboard \
        at-spi2-core gsettings-desktop-schemas gir1.2-atspi-2.0 \
        xdg-utils xdg-user-dirs \
        x11-xkb-utils xkb-data \
        locales keyboard-configuration \
        fonts-dejavu fonts-liberation fonts-noto-core \
        python3 python3-gi gir1.2-gtk-3.0 libglib2.0-bin \
        libsdl2-2.0-0 \
        polkitd pkexec \
        sudo \
    && rm -rf /var/lib/apt/lists/*\
    && sed -i 's/^# *\(en_US.UTF-8 UTF-8\)/\1/' /etc/locale.gen \
    && locale-gen

# Dedicated unprivileged user; its home is the persistent volume.
RUN useradd -m -u 1000 -s /bin/bash wup \
    && mkdir -p /home/wup/.config/wup-thinserver \
    && chown -R wup:wup /home/wup

# Daemon, settings application, desktop metadata, session entrypoint and
# helper scripts — all installed by `meson install` into /out/usr/local.
COPY --from=builder /out/usr/local/ /usr/local/

# State dirs the session needs, owned by wup.
RUN mkdir -p /tmp/pulse-runtime /tmp/xdg-runtime \
    && chmod 700 /tmp/pulse-runtime /tmp/xdg-runtime \
    && chown wup:wup /tmp/pulse-runtime /tmp/xdg-runtime

WORKDIR /home/wup

# Healthcheck: X display answers AND the daemon is alive (as user wup).
HEALTHCHECK --interval=15s --timeout=5s --start-period=45s --retries=5 \
    CMD runuser -u wup -- bash -c 'DISPLAY=:99 xdpyinfo >/dev/null 2>&1 && pgrep -x wup-thinserver >/dev/null'

ENTRYPOINT ["/usr/local/bin/wup-thin-entrypoint"]
