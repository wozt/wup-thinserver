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
        libglib2.0-dev libx11-dev libxrandr-dev libxtst-dev libxext-dev \
        libxinerama-dev libxcb1-dev libxcb-shm0-dev libx11-xcb-dev \
        libpulse-dev libcairo2-dev libpango1.0-dev libjson-glib-dev \
        libswscale-dev libdrm-dev libgbm-dev libegl-dev libgles-dev \
        nasm \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN meson setup build --buildtype=release \
    && ninja -C build \
    && install -Dm755 build/wup-thinserver /out/wup-thinserver

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
    WUP_THIN_CONFIG=/etc/wup-thinserver/settings.json \
    PULSE_RUNTIME_PATH=/tmp/pulse-runtime \
    XDG_RUNTIME_DIR=/tmp/xdg-runtime

# Runtime libraries the daemon links against (built in stage 1).
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        libglib2.0-0 libx11-6 libxrandr2 libxtst6 libxext6 libxinerama1 \
        libxcb1 libxcb-shm0 libx11-xcb1 \
        libpulse0 libcairo2 libpangocairo-1.0-0 libjson-glib-1.0-0 \
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
        xdg-utils \
        x11-xkb-utils xkb-data \
        locales keyboard-configuration \
        fonts-dejavu fonts-liberation fonts-noto-core \
        python3 python3-gi gir1.2-gtk-3.0 \
        polkitd pkexec \
        sudo \
    && rm -rf /var/lib/apt/lists/*\
    && sed -i 's/^# *\(en_US.UTF-8 UTF-8\)/\1/' /etc/locale.gen \
    && locale-gen

# Dedicated unprivileged user; its home is the persistent volume.
RUN useradd -m -u 1000 -s /bin/bash wup \
    && mkdir -p /home/wup/.config/wup-thinserver \
    && chown -R wup:wup /home/wup

# Daemon binary + helpers + default config.
COPY --from=builder /out/wup-thinserver /usr/local/bin/wup-thinserver
COPY config/settings.example.json /etc/wup-thinserver/settings.json
COPY scripts/onboard-toggle.sh /usr/local/bin/onboard-toggle
RUN chmod 755 /usr/local/bin/onboard-toggle \
    && chown -R wup:wup /etc/wup-thinserver

# Session entrypoint (root -> drops to wup via gosu-less setpriv? we run the
# whole container as wup; see compose `user:`). We keep files root-owned where
# they must stay read-only.
COPY docker/entrypoint.sh /usr/local/bin/wup-thin-entrypoint
RUN chmod 755 /usr/local/bin/wup-thin-entrypoint

# State dirs the session needs, owned by wup.
RUN mkdir -p /tmp/pulse-runtime /tmp/xdg-runtime \
    && chmod 700 /tmp/pulse-runtime /tmp/xdg-runtime \
    && chown wup:wup /tmp/pulse-runtime /tmp/xdg-runtime

USER wup
WORKDIR /home/wup

# Healthcheck: X display answers AND the daemon is alive.
HEALTHCHECK --interval=15s --timeout=5s --start-period=45s --retries=5 \
    CMD bash -c 'DISPLAY=:99 xdpyinfo >/dev/null 2>&1 && pgrep -x wup-thinserver >/dev/null'

ENTRYPOINT ["/usr/local/bin/wup-thin-entrypoint"]
