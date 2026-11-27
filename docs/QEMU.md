# QEMU ARM64 Appliance

This guide covers the bootable Debian 13 ARM64 QEMU image that runs the
native ThinServer installation (XFCE desktop streamed over WUP, Onboard
on-screen keyboard, WUP ThinServer Settings, virtual audio sink).

## Prerequisites (Debian 13 AMD64 host)

```sh
sudo apt install qemu-system-arm qemu-efi-aarch64 qemu-user \
    cloud-image-utils xorriso debootstrap
aarch64-linux-gnu-gcc   # gcc-aarch64-linux-gnu; plus :arm64 -dev libs
```

The ARM64 development libraries (`libasound2-dev:arm64`,
`libx11-dev:arm64`, `libgtk-3-dev:arm64`, `libxfixes-dev:arm64`,
`libx264-dev:arm64`, `libpulse-dev:arm64`, ...) are listed in
`provisioning/packages/build-arm64.txt` and installed by
`scripts/build-arm64.sh` prerequisites. `qemu-aarch64-static` binfmt
must be registered (`qemu-user-static` / `qemu-user-binfmt`) to run the
built binaries for smoke tests.

TCG emulation is used — there is no ARM64 KVM on an AMD64 host. Expect
slow boots and slow desktop interaction inside the guest.

## Build the image

```sh
./scripts/build-qemu-image.sh
```

What it does:

1. Downloads `debian-13-genericcloud-arm64.qcow2` from
   `cloud.debian.org` (pinned release, SHA-512 verified; see
   `scripts/build-qemu-image.sh` for the exact version/checksum).
2. Cross-compiles `wup-thinserver` and `wup-thinserver-settings` for
   aarch64 into `build/images/qemu-arm64-payload/` (including the
   patched x264 GamePad encoder build via the arm64 dependency
   toolchain).
3. Generates a per-build SSH admin key pair
   (`build/images/qemu-admin-key`, git-ignored).
4. Builds a minimal cloud-init seed (`build/images/seed.img`): admin
   user + SSH key + hostname only.
5. Resizes the disk to 20 GiB, writes
   `build/images/wup-thinserver-qemu-arm64.qcow2`, then provisions it
   OFFLINE via `scripts/provision-qemu-image.sh`: the image is mounted
   with qemu-nbd and `images/qemu/provision-guest.sh` runs inside an
   arm64 chroot (qemu-aarch64-static binfmt). arm64 apt under qemu-user
   takes ~10 minutes vs ~1 h+ for first-boot apt under full-system TCG.
   `SKIP_PROVISION=1` skips this step (cloud-init fallback removed —
   always provision offline).

Re-running the script re-provisions from the pristine cloud image;
existing VM data is kept only if you reuse the qcow2 (the script
recreates it — back it up first if it holds state you want).

## Launch

```sh
./scripts/run-qemu.sh                 # functional profile: 4 vCPU, 4 GiB
./scripts/run-qemu.sh --profile pi4   # cortex-a72, 4 vCPU, 2 GiB
./scripts/run-qemu.sh --profile zero2w# cortex-a53, 4 vCPU, 512 MiB
./scripts/stop-qemu.sh                # graceful QMP shutdown
./scripts/qemu-console.sh             # attach to serial console
```

Headless by design: `-display none`, serial console on
`build/run/serial.log` (also a unix socket `build/run/serial.sock`).
QMP and HMP sockets live in `build/run/`.

Environment overrides: `WUP_QEMU_HOST_ADDR` (default `0.0.0.0`),
`WUP_QEMU_HOST_PORT` (default `5080`), `WUP_QEMU_GUEST_PORT`
(default `50001`), `WUP_QEMU_SSH_PORT` (default `2222`, `0` disables),
`WUP_QEMU_MEM`, `WUP_QEMU_CPUS`.

## Networking

QEMU user-mode networking (slirp) forwards:

- `udp host:5080 -> guest:50001` — the WUP GamePad stream endpoint
- `tcp host:2222 -> guest:22` — admin SSH (key
  `build/images/qemu-admin-key`)

Because the server now answers from the same socket the client dialed,
replies leave the host from `:5080`, which is exactly the endpoint the
GamePad must be configured with.

### GamePad connection profile

- Profile name: `ARM64 Lab`
- Host: the LAN IP of this workstation (e.g. `192.168.x.y` — check with
  `hostname -I`; do not hard-code it)
- Port: `5080`
- Auto-connect: disabled

Example SSH access:

```sh
ssh -i build/images/qemu-admin-key -p 2222 admin@127.0.0.1
```

## What runs inside the guest

A single supervised unit, `wup-thinserver.service`
(`packaging/systemd/wup-thinserver.service`), runs
`wup-thin-entrypoint --session` as the unprivileged `wup` user. That
entrypoint (`provisioning/session/entrypoint.sh`, shared with the
Docker image) brings up, in order:

1. `/run/wup` runtime dirs with correct ownership,
2. private D-Bus session bus,
3. Xvfb virtual display (default `:99`, 1280x720),
4. PulseAudio null sink (private runtime dir),
5. XFCE session,
6. Onboard on-screen keyboard,
7. the `wup-thinserver` streaming daemon,

with clean shutdown of the whole tree on stop and
`Restart=on-failure`. The desktop stays up when the GamePad
disconnects; the daemon does not depend on any physical display.
Guest-side tunables (`DESKTOP_WIDTH`, `DESKTOP_HEIGHT`,
`DISPLAY_NUM`, `KEYBOARD_LAYOUT`, `WUP_THIN_CONFIG`) are set through a
systemd drop-in written by `images/qemu/provision-guest.sh`.

## Validation performed (this build)

- Guest boots Debian 13 arm64 to multi-user in ~60 s wall time (TCG).
- `uname -m` = aarch64; `systemd-networkd`, `ssh`, `wup-thinserver`
  all `active`; daemon runs as `wup`.
- XFCE session up as `wup`: xfce4-session, xfwm4, xfce4-panel,
  xfdesktop, Thunar, Onboard (pid visible), private PulseAudio,
  private D-Bus; Xvfb display :99 at 1280x720 (xwininfo verified).
- Desktop shortcut `wup-thinserver-settings.desktop` on `/home/wup/Desktop`;
  `/usr/local/bin/wup-thin{server,-settings,-entrypoint}` present.
- WUP protocol through slirp UDP forward (host :5080 → guest :50001):
  `fake_pad.py --server 127.0.0.1 --port 5080 --seconds 25 --send-t
  --send-n --keyframe` → `RESULT ok=1 hellos=85 videos=1689
  uniq_frames=257 audios=588 t_acks=1 peer=127.0.0.1:5080` — replies
  originate from the dialed host endpoint.
- Persistence: file in /home/wup + settings.json change survived a
  graceful poweroff + reboot; services auto-started.
- Admin SSH: `ssh -i build/images/qemu-admin-key -p 2222 admin@127.0.0.1`.
- Screenshot evidence: `docs/qemu-evidence/desktop-1280x720.png`
  (xwd of :99 captured inside the guest).

### Gotcha fixed during bring-up

An earlier payload tarball archived `.` from a `mktemp -d` (mode 700,
host uid) — extraction chmod'ed the guest `/` to `drwx------` owned by
the `wup` user, which broke every non-root service (systemd-networkd
exit 200/CHDIR → no DHCP → no network at all). The builder now tars
explicit top-level dirs with `--owner=0 --group=0`. If a hand-built
image shows `ls -ld /` != `drwxr-xr-x root root`, fix it offline before
booting.

## Limitations

- TCG emulation: the desktop is usable but slow; do not use QEMU FPS
  numbers as Raspberry Pi performance evidence.
- `pi4`/`zero2w` profiles approximate CPU/ABI only — they do not model
  Pi firmware, GPU, or codec blocks.
- slirp UDP forwarding is per-flow; symmetric NAT and multiple
  simultaneous GamePads are not supported by design (one active
  session).
- The QEMU graphical console is not used; diagnostics go through the
  virtual X display inside the guest (xwd/import via SSH).
