# Installation & deployment

Three deployment profiles share the same provisioning:

| Profile | Method | Status |
|---|---|---|
| AMD64 Docker | `docker compose up -d` | primary, in progress |
| Debian / Raspberry Pi OS native | `scripts/install-native.sh` (planned) | planned |
| Raspberry Pi images | generated from the same provisioning | planned |

## Docker (primary)

### Build and run

```sh
docker compose up -d --build     # first build takes a while (desktop + x264)
docker compose ps
docker compose logs -f wup-thinserver
```

### Common commands

```sh
docker compose stop              # stop container (session processes end)
docker compose start             # start again
docker compose restart           # restart
docker compose down              # stop and remove container (volume kept)
docker compose up -d --build     # rebuild after pulling changes
docker compose logs -f           # logs
docker compose exec --user wup wup-thinserver bash   # shell as the desktop user
```

The entrypoint starts as root only to provision the `wup` account, then drops
to `wup` via `setpriv` — the session itself is never root. `docker compose
exec` defaults to root; use `--user wup` (or `docker exec -u wup`) for a
desktop-user shell, `-u root` for administration.

### Hostname, account and password

- The container hostname defaults to **`wup-thinserver`** (`hostname` inside,
  terminal prompt `wup@wup-thinserver:~$`). Override with `THIN_HOSTNAME` in
  `.env`. The host machine's hostname is untouched.
- The desktop user is **`wup`** with home `/home/wup` (persistent volume).
- By default the account has a **locked password**: no console login, no
  sudo. The remote desktop does not need it — the GamePad is the only
  intended input path.

Set a password (pick one):

```sh
# a) one-shot env (not persisted across `down`; use .env to persist)
#    .env:  WUP_PASSWORD=...   or   WUP_PASSWORD_HASH=$(openssl passwd -6 '...')
docker compose up -d

# b) after the container runs (interactive, never logged):
docker exec -it -u root wup-thinserver wup-set-password

# c) password hash without a TTY:
docker exec -u root wup-thinserver usermod -p '$(openssl passwd -6 SECRET)' wup
```

Optional in-container sudo (does **not** affect the host; the container is
the isolation boundary):

```sh
# .env: WUP_SUDO=1            -> wup in the sudo group (sudo asks the password)
#      WUP_SUDO_NOPASSWD=1    -> passwordless sudo inside the container only
docker compose up -d
```

### GamePad (virtual controller) mode

Desktop mode needs nothing extra. For Gamepad mode — the native virtual
evdev controller that games and RetroArch-style emulators can use — add
the optional overlay on hosts with `/dev/uinput` (kernel `uinput` module):

```sh
docker compose -f docker-compose.yml -f docker-compose.gamepad.yml up -d --build
```

What the overlay does (`docker-compose.gamepad.yml` documents each part):

- bind-mounts `/dev/uinput` (no `privileged`, no host `/dev/input`);
- lets the entrypoint `mknod` a private `/dev/uinput-wup` node so the
  host's own uinput permissions are untouched;
- starts `wup-gamepad-devd`, a small watcher that creates
  `/dev/input/eventN` for `WUP ThinServer Gamepad` so SDL/evdev apps can
  open it, and removes stale nodes after mode switches;
- adds `libsdl2-2.0-0` to the image so SDL applications can enumerate the
  controller.

What it does **not** do — read before relying on it:

- The virtual controller is created in the **shared host kernel**: SDL and
  evdev applications running on the host itself can also see
  `WUP ThinServer Gamepad`. Docker isolates the device *nodes* visible
  inside the container; it does not virtualize the kernel input
  subsystem. There is no complete input-device isolation.
- Physical host keyboards, mice and controllers are **not** mounted into
  the container; only the single virtual node is reachable.
- Hosts without uinput keep working: Desktop mode is unaffected and
  Gamepad mode reports the probe error in Settings. Set `WUP_GAMEPAD=0`
  to disable the overlay even when `/dev/uinput` is present, or point
  `WUP_UINPUT_PATH` at a differently named node.

Switch modes live from the GamePad (HOME+X) or in Settings — the choice
is persisted and applies without a restart.

### Installing extra packages

```sh
docker exec -u root wup-thinserver apt-get update
docker exec -u root wup-thinserver apt-get install -y <pkg>
```

- Survives `docker compose restart` / `stop`+`start` (container filesystem
  is kept).
- **Lost on `docker compose down` + `up`** (container recreated). For
  permanent additions either add the package to `docker/Dockerfile` (and
  rebuild) or commit a derived image — runtime `apt` installs are not part
  of the persistent `wup-home` volume.

### Configuration migration

On every start the entrypoint runs idempotent migrations against the
persistent home volume — no `down -v` needed for software updates:

- `wup-migrate-settings` rewrites `~/.config/wup-thinserver/settings.json`
  to the current schema (desktop controller bindings, Onboard command) and
  keeps a `.pre-desktop-migration.bak` backup.
- `onboard-setup` applies the docked/auto-show Onboard defaults once
  (marker file); `ONBOARD_*` env vars override on every start.
- `wup-install-desktop-shortcut` adds WUP ThinServer Settings to the localized
  XDG desktop when absent, marks it executable/trusted, and never overwrites a
  customized launcher.
- Documents, Firefox profiles, downloads and unrelated XFCE settings are
  never touched.

### Persistent data

The GamePad user's home (`/home/wup`) is a named volume `wup-home`:
Firefox profiles, bookmarks, downloads, XFCE settings, Onboard settings and
the daemon's `settings.json` survive container recreation.

```sh
# backup
docker run --rm -v wup-thinserver_wup-home:/data -v "$PWD":/backup \
    alpine tar czf /backup/wup-home-$(date +%F).tgz -C /data .

# reset the desktop environment (keeps the image)
docker compose down
docker volume rm wup-thinserver_wup-home
docker compose up -d

# uninstall
docker compose down -v          # also deletes the home volume
docker rmi wup-thinserver:latest
```

### Networking

The container uses bridge networking (default compose network). The daemon
needs:

- UDP **50001** broadcast listener (pad discovery, bound on the container's
  interfaces) — GamePad beacons reach it because the GamePad broadcasts.
- UDP replies to the GamePad's source port (ephemeral host port → pad :50000).

Outbound UDP to the LAN works through NAT; inbound discovery relies on the
GamePad's broadcast `B` packets, which Docker's bridge forwards only if the
container shares the host network **or** the pad's subnet broadcast reaches
the bridge. For the first prototype `network_mode: host` is used — see
`docker-compose.yml` comments for the rationale and how to switch to bridge
with `macvlan`/`ipvlan` later.

> Port conflicts: WUP ThinServer and the original WUP Stream server both
> listen on UDP 50001 for discovery and send to pad :50000. They can coexist
> (each binds its own ephemeral socket; `SO_REUSEPORT`/`SO_REUSEADDR` is set
> on the discovery socket), but a pad pairs with whichever server answers —
> do not run both against the same GamePad.

### Inspecting the virtual display (testing without a GamePad)

```sh
docker compose exec wup bash
DISPLAY=:99 xwd -root -silent | xwdtopnm | pnmtopng > /tmp/screen.png   # needs x11-apps/netpbm
# or simpler, inside the image:
DISPLAY=:99 import -window root /tmp/screen.png                        # imagemagick (optional)
ffmpeg -f x11grab -video_size 1280x720 -i :99 -frames:v 1 /tmp/shot.png
```

`x11vnc` can be added for interactive debugging (not installed by default).

## Native installation (planned, Milestone 7)

`scripts/install-native.sh` will:

- create the `wup` session user,
- install the same package set and desktop provisioning,
- install a systemd user service `wup-thinserver.service` and a session
  launcher `wup-thin-session.service`,
- never touch the user's existing desktop or X session,
- provide `wup-thin-uninstall.sh`.

The desktop resolution is configured once (env `DESKTOP_WIDTH`,
`DESKTOP_HEIGHT`, `DESKTOP_DEPTH`) and shared by Docker and native installs.

## Security notes

- No authentication or encryption: anyone on the LAN who can reach the
  daemon's UDP ports can view the desktop and inject input. **Trusted
  networks only.**
- The container is unprivileged (`USER wup`), no `--privileged`, no host
  Docker socket, no host X11 (`/tmp/.X11-unix`) or PulseAudio socket mounts.
- Firefox keeps its sandbox and security features; the container adds no
  `--no-sandbox` flags.
- Input injection uses XTest into the session display only.
- `/dev/uinput` virtual-gamepad mode is optional and off by default
  (`input.mode = 1`, kb/mouse). If enabled it needs `uinput` group access.
