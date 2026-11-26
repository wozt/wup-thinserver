# WUP ThinServer

WUP ThinServer is a headless Linux daemon that turns a Linux machine into an
independent desktop session for the [WUP ThinClient](https://github.com/wozt/wup-thinclient)
— a homebrew app for the Wii U GamePad (WUP-010).

The GamePad becomes a thin client: it displays the remote desktop, plays its
audio, and forwards its touchscreen and buttons. All Linux applications
(Firefox ESR, Thunar, XFCE Terminal, Onboard on-screen keyboard, ...) run
inside the session managed by ThinServer.

## How it differs from WUP Stream

- **WUP Stream** streams an *existing* computer desktop or window that is
  already running on the host, through a GTK settings application.
- **WUP ThinServer** owns its *own* Linux desktop session: a dedicated X11
  display (Xvfb), its own D-Bus and audio sessions, XFCE, and persistent user
  data. It requires no monitor, no host desktop and no host audio.

Both share the same wire protocol and encoder (`libwupstream`, WUP Stream), so
a WUP ThinClient can in principle also receive a stream from a WUP Stream
server, and the original WUP Stream pad app can connect to ThinServer.

## Architecture (summary)

```
Wii U GamePad (WUP ThinClient)
        |  UDP :50000/:50001  (WUP Stream protocol, H.264 + PCM audio)
        v
wup-thinserver daemon  --X11-->  session display (Xvfb :99)
        |                            |
        +-- PulseAudio monitor <-----+-- XFCE session (dbus, pulse, onboard)
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for details.

## Quick start

### Docker (recommended, primary deployment)

Prerequisites: Docker Engine + Compose v2. No other dependencies.

```sh
git clone https://github.com/wozt/wup-thinserver
cd wup-thinserver
docker compose up -d --build     # build image + start session
docker compose logs -f           # follow logs
docker compose ps                # status / healthcheck
docker compose down              # stop (home data persists in volume wup-home)
docker compose up -d             # start again
```

Environment overrides (see `.env.example` and docs/CONFIGURATION.md):
`THIN_HOSTNAME` (default `wup-thinserver`), `DESKTOP_WIDTH`,
`DESKTOP_HEIGHT`, `KEYBOARD_LAYOUT`, `KEYBOARD_VARIANT`, `LOCALE`,
`DISPLAY_NUM`, `WUP_PASSWORD`/`WUP_PASSWORD_HASH`, `WUP_SUDO`,
`WUP_SUDO_NOPASSWD`, `ONBOARD_DOCK_HEIGHT`, `ONBOARD_LAYOUT`.
Persistent data: `docker volume inspect wup-thinserver_wup-home`
(backup = copy the volume; reset = `docker compose down -v`).

Inside the session: user `wup`, hostname `wup-thinserver`, XFCE desktop,
Onboard on-screen keyboard (auto-shows on text fields, GamePad **X** toggles
it), Firefox ESR, Thunar, Mousepad, xfce4-terminal.

The desktop also contains **WUP ThinServer Settings**. Open its desktop or
application-menu launcher to change controller bindings, stick behavior,
touch, gyro, audio input options, and the active Desktop/Gamepad input mode.
Valid changes take effect immediately
and are saved without restarting the daemon, stream, XFCE, or container. See
[the settings application](docs/SETTINGS_APPLICATION.md) and
[local control protocol](docs/CONTROL_IPC.md).

### Native build (Debian 13 / compatible)

Prerequisites: `meson ninja-build pkg-config gcc` plus the X11/XCB,
PulseAudio, cairo/pango, json-glib and swscale development packages
(listed in `docker/Dockerfile` and docs/BUILD.md).

```sh
meson setup build                    # GTK settings app: auto-detected
meson compile -C build -j8
meson test -C build --print-errorlogs     # 6 tests incl. IPC, capture and evdev/SDL
./build/wup-thinserver --check            # validate config
./build/wup-thinserver                    # needs DISPLAY of a session
./build/wup-thinserver-settings           # needs GTK3 and the running daemon
```

For a daemon-only build, configure with `-Dsettings_app=disabled`.

The build fetches and pins `libwupstream` from `wozt/wup-stream` itself —
no WUP Stream checkout required.

### Connect

Start **WUP ThinClient** on the GamePad (same LAN). It opens a connection
menu — nothing streams until you press **Connect** (or enable
auto-connect). Edit the server IP there (**Edit server address**), or leave
"any server" to take the first ThinServer that answers. While streaming,
**HOME** opens the local menu (resume / disconnect / reconnect / controls /
exit); **HOME+B** held for 2 s quits the app.

Controller defaults (all configurable — see
`config/settings.example.json` and docs/CONFIGURATION.md):

| GamePad | Action |
|---|---|
| Touch | absolute pointer + tap / drag / scroll / long-press right-click |
| Left stick | mouse move |
| Right stick | scroll |
| ZL / L3 | left mouse button |
| ZR / R3 | right mouse button |
| A / B | Enter / Escape |
| X | toggle Onboard keyboard |
| Y | Alt+Tab |
| L / R | previous / next workspace |
| D-pad | arrow keys |
| + / − | zoom |
| TV | fullscreen |

See [docs/INSTALLATION.md](docs/INSTALLATION.md),
[docs/BUILD.md](docs/BUILD.md), [docs/CONFIGURATION.md](docs/CONFIGURATION.md).

## Status

Early prototype. See [docs/PROJECT_STATUS.md](docs/PROJECT_STATUS.md) for what
is implemented, tested and known-broken.

## License

GPL-3.0-or-later — see [LICENSE](LICENSE). Vendored components keep their
original copyright; see [docs/UPSTREAM.md](docs/UPSTREAM.md).
