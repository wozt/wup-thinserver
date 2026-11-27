# Raspberry Pi images

WUP ThinServer uses the same native architecture on Raspberry Pi, QEMU and
Debian: one `wup-thinserver.service` owns one unprivileged Xvfb/XFCE desktop,
PulseAudio null sink, Onboard, Settings and daemon. Docker is not installed
or required on the images.

## Image generator selection

The image pipeline pins Raspberry Pi's `rpi-image-gen` v2.8.0 commit
`262d4df5a9f9d4133370465399a7958a7c22cdc7`. It was selected over `pi-gen`
because it has official `rpi4` and `rpizero2w` device layers, a native Debian
13/Trixie ARM64 suite, external layers and rootfs overlays, metadata
validation, and deterministic composable configuration. `pi-gen` remains the
tool used to build official Raspberry Pi OS distribution images, but its
ordered stage model would duplicate more of ThinServer's existing native
provisioning.

Primary sources:

- [rpi-image-gen](https://github.com/raspberrypi/rpi-image-gen/tree/v2.8.0)
- [rpi-image-gen layers](https://github.com/raspberrypi/rpi-image-gen/blob/v2.8.0/docs/layer/index.adoc)
- [pi-gen](https://github.com/RPi-Distro/pi-gen)

The generator supports cross-building on recent Debian AMD64 through QEMU,
but Raspberry Pi documents native Debian/Raspberry Pi OS ARM64 as the
supported host. It needs private user and mount namespaces; the supported
rootless path uses Podman, while restricted build containers may need extra
namespace capabilities. The build script never installs host dependencies
automatically.

## Build

Prepare the pinned source, install the dependencies reported by its official
installer, then run the image builds:

```sh
./scripts/build-rpi-image.sh pi4 --prepare-only
sudo build/rpi-image-gen/source/install_deps.sh
./scripts/build-rpi-image.sh pi4
./scripts/build-rpi-image.sh zero2w
```

Use `--prepare-only` to cross-build the application, resolve the shared
package lists and stage a complete external rpi-image-gen project without
mounting or creating an image.

The image hook invokes `scripts/install-native.sh --prebuilt` for the account,
settings, installed payload and systemd service. `--skip-packages` only avoids
running APT a second time after `rpi-image-gen` has installed the package lists
from `provisioning/packages/`; it does not introduce a parallel provisioning
implementation.

Outputs are git-ignored:

```text
build/images/wup-thinserver-pi4-arm64.img.xz
build/images/wup-thinserver-pi4-arm64.img.xz.sha256
build/images/wup-thinserver-zero2w-arm64.img.xz
build/images/wup-thinserver-zero2w-arm64.img.xz.sha256
build/images/rpi-admin-key
```

The image set gets a build-local SSH key when one does not already exist. The
`admin` and `wup` passwords remain locked; root login and password SSH are
disabled. Preserve the private key securely or replace the public key in each
prepared project before building.

## Profiles

| Profile | Device layer | Desktop | Port | Root image | Status |
|---|---|---:|---:|---:|---|
| Pi 4 | `rpi4` | 1280×720 | 50004 | 8 GiB | Primary; build validation does not prove board compatibility or FPS |
| Zero 2 W | `rpizero2w` | 854×480 | 50005 | 6 GiB | Experimental 512 MiB profile; physical resource measurements pending |

The Zero 2 W is genuinely reduced: it uses a separate package list without
Thunar, Mousepad, X11 demo applications, Polkit or extra font families;
compositing is disabled; optional background maintenance/discovery/printing
services are masked. Firefox ESR and Onboard remain because they are part of
the requested ThinServer desktop. Camera and browser concurrency must be
measured on a physical 512 MiB board before making usability claims.

Both profiles preserve Raspberry Pi firmware, its 64-bit kernel and hardware
support selected by the official device layer. No HDMI desktop or display
manager is installed or enabled. The graphical session exists only on Xvfb.

## Virtual devices

`uinput` is loaded at boot and a narrow udev rule gives the `wup` user's
`input` group access. No physical input tree is exposed by another container.

Camera Preview needs no virtual webcam. Announce optionally requires a
v4l2loopback module built for the *running Raspberry Pi kernel*:

```sh
sudo /usr/local/bin/wup-prepare-native-camera --install
```

This is explicit and is never run on ordinary boot. On 64-bit Raspberry Pi
OS it installs `linux-headers-rpi-v8`, as required by the
[Raspberry Pi kernel documentation](https://www.raspberrypi.com/documentation/computers/linux_kernel.html#kernel-headers),
then the Trixie v4l2loopback DKMS/utilities packages. It refuses to overwrite
an occupied `/dev/video42`, chooses the next free number, creates the exact
`Wii U GamePad Camera` identity, and reports the `camera_device` value to use.
Generic Debian instead uses headers matching `uname -r`; generic headers must
not be substituted for a Raspberry Pi downstream kernel. If another
v4l2loopback instance is already loaded, the helper refuses to unload or
replace it.

## Validation boundaries

The existing QEMU `virt` appliance validates ARM64 user space, not Pi
firmware, VideoCore, Wi-Fi or performance. QEMU's `raspi4b` model is useful
only for early boot experiments and still cannot establish physical board
compatibility. A milestone is physically validated only after flashing the
`.img.xz`, booting the intended board and recording stream, memory, CPU,
camera and reconnect evidence.

The generated Pi 4 image has an automated, isolated early-boot smoke test:

```sh
./scripts/test-rpi-image-qemu.sh
```

It extracts the image's own Raspberry Pi kernel, initramfs and DTB, adds a
temporary 16 GiB copy-on-write SD wrapper (the `raspi4b` model requires a
power-of-two card size), and boots without host networking. Success means the
real root partition mounted, `wup-thinserver.service` started and
`multi-user.target` was reached. Test-only kernel arguments mask the boot
partition and network-online waits that QEMU cannot satisfy. VideoCore,
EEPROM, codec, Wi-Fi, streaming performance and physical compatibility remain
outside this test.

On 2026-11-27 both profiles completed a cross-host build and passed `xz -t`,
SHA-256 verification and offline filesystem inspection. The Pi 4 image also
passed the `raspi4b` smoke test with Raspberry Pi kernel
`6.18.60+rpt-rpi-v8`. The generated files were 353 MiB (Pi 4) and 328 MiB
(Zero 2 W) for that build; size and checksum can change with repository
updates and regenerated filesystem identifiers.

