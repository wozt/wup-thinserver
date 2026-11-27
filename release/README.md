# WUP ThinServer — Installation

WUP ThinServer is a headless streamed-desktop appliance for the Wii U
GamePad ThinClient: one service owns a virtual XFCE desktop (Xvfb), a
PulseAudio null sink, Onboard and the streaming daemon — no physical
display or login manager required.

Pick the artifact that matches your target:

| Artifact | Target | Contents |
|---|---|---|
| `wup-thinserver-pi4-arm64.img.xz` | Raspberry Pi 4 SD card | Debian 13 appliance, headless |
| `wup-thinserver-zero2w-arm64.img.xz` | Raspberry Pi Zero 2 W | Reduced 512 MiB Debian 13 appliance |
| `wup-thinserver-pios-pi4-arm64.img.xz` | Raspberry Pi 4 SD card | Raspberry Pi OS (Trixie) appliance |
| `wup-thinserver-pios-zero2w-arm64.img.xz` | Raspberry Pi Zero 2 W | Raspberry Pi OS (Trixie) appliance |
| `wup-thinserver-<ver>-<arch>.zip` | Any Debian 13 PC | Native installer bundle |
| `wup-thinserver_<ver>_<arch>.deb` | Debian 13 (amd64/arm64) | APT package |
| `wup-thinserver-<ver>-<arch>.AppImage` | Any Linux desktop | Settings app + daemon binaries |
| `docker-compose.yml` | Docker host | Container deployment (ghcr.io image) |

Every artifact ships `SHA256SUMS`-style `.sha256` files; verify with
`sha256sum -c <file>.sha256` after download.

## Raspberry Pi images

1. Flash the `.img.xz` to an SD card (decompress first: `unxz` or
   Raspberry Pi Imager accepts `.xz` directly):

   ```sh
   unxz wup-thinserver-pi4-arm64.img.xz
   # then write the .img to the card (dd, Raspberry Pi Imager, balenaEtcher…)
   ```

2. Boot the Pi on your LAN. The appliance listens for GamePad sessions on
   its dedicated UDP port (50004 for Pi 4, 50005 for Zero 2 W).

### Accounts and SSH

- `wup` / `wup` — appliance user, member of `sudo`
- `root` / `wup`
- `admin` — SSH key only (the build-local `rpi-admin-key` is not shipped;
  add your own key to `/home/admin/.ssh/authorized_keys` from console or
  via `wup-firstboot.conf`)
- SSH is enabled by default **with password authentication**:
  `ssh wup@<pi-ip>`

### Wi-Fi and first-boot configuration (headless)

The FAT32 **BOOT** partition of the card is visible from any PC. It ships
`wup-firstboot.conf.example`. To configure Wi-Fi, passwords or hostname
without rebuilding the image:

1. Copy `wup-firstboot.conf.example` to `wup-firstboot.conf` on the BOOT
   partition.
2. Edit it:

   ```ini
   wifi_ssid=YourNetworkName
   wifi_psk=your-passphrase
   # password=new-wup-password
   # root_password=new-root-password
   # hostname=my-thinserver
   # ssh=off
   # ssh_password_auth=no
   ```

3. Boot the Pi — `wup-firstboot.service` applies the file before
   networking comes up, and re-applies it whenever the file changes.

Supported keys: `wifi_ssid`, `wifi_psk` (passphrase or 64-hex PSK; omit
for open networks), `password`, `root_password`, `hostname`,
`ssh` (`on`/`off`), `ssh_password_auth` (`yes`/`no`).

Note: Raspberry Pi Imager's built-in OS customization targets Raspberry
Pi OS (`userconf.txt`, netplan `network-config`, `wpa_supplicant.conf`)
and does **not** apply to these Debian images — `wup-firstboot.conf` is
the supported equivalent. Ethernet DHCP works out of the box on `eth0`.

## Native Linux install (zip)

```sh
unzip wup-thinserver-<ver>-<arch>.zip
cd wup-thinserver-<ver>-<arch>
sudo ./install.sh                 # add --password, --port N, --desktop WxH
```

Requires Debian 13 (trixie) systemd on amd64 or arm64 with network access;
the installer pulls the desktop session packages via APT, creates the
`wup` account and enables `wup-thinserver.service` at boot.

## Debian package (.deb)

```sh
sudo apt install ./wup-thinserver_<ver>_<arch>.deb
```

APT resolves the full dependency set (XFCE session, PulseAudio, Onboard,
Firefox ESR, codecs). The postinst creates the `wup`/`wup` account,
configures the service and starts it. Same options as the zip afterwards:
`systemctl edit wup-thinserver` for drop-in overrides.

## AppImage

```sh
chmod +x wup-thinserver-<ver>-<arch>.AppImage
./wup-thinserver-<ver>-<arch>.AppImage          # GTK Settings app
./wup-thinserver-<ver>-<arch>.AppImage daemon   # streaming daemon
```

The AppImage carries the Settings app and daemon binaries only — the full
Xvfb/XFCE session still requires a system install (deb/zip/native).

## Docker

```sh
docker login ghcr.io            # if the package is private
docker compose up -d            # uses ghcr.io/wozt/wup-thinserver:<tag>
```

`docker-compose.yml` runs the appliance in host network mode (required by
the GamePad discovery protocol). Optional overlays:
`docker-compose.gamepad.yml` (virtual gamepad via `/dev/uinput`) and
`docker-compose.camera.yml` (v4l2loopback camera). See `docs/` in the
repository for the full configuration reference.

## Verifying downloads

Each asset has a `.sha256` sidecar:

```sh
sha256sum -c wup-thinserver-pi4-arm64.img.xz.sha256
```
