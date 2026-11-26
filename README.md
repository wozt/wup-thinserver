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

## Quick start (Docker, recommended)

```sh
docker compose up -d --build     # build and start
docker compose logs -f           # follow logs
docker compose down              # stop (session data persists in a volume)
```

Then start **WUP ThinClient** on the GamePad and connect to this machine's IP.

See [docs/INSTALLATION.md](docs/INSTALLATION.md) and
[docs/BUILD.md](docs/BUILD.md).

## Status

Early prototype. See [docs/PROJECT_STATUS.md](docs/PROJECT_STATUS.md) for what
is implemented, tested and known-broken.

## License

GPL-3.0-or-later — see [LICENSE](LICENSE). Vendored components keep their
original copyright; see [docs/UPSTREAM.md](docs/UPSTREAM.md).
