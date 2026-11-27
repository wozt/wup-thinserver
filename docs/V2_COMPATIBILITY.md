# WUP ecosystem V2 compatibility

Audit date: 2026-11-27.

This audit covers all 40 public repositories returned by the `wozt` GitHub
account plus relevant local checkouts. Seven public repositories participate
in the WUP Homebrew or WUP Stream ecosystem. `wozt-gtk-standard` is a UI
reference only. Other WOZT repositories do not use libwup, libwupstream,
NetFS, or the WUP Stream wire protocol.

## Compatibility matrix

| Project | Audited revision | Dependency / SDK state | V2 and camera state | Build / local state |
|---|---|---|---|---|
| [wup-homebrew](https://github.com/wozt/wup-homebrew) | `feature/netfs-v2` `3da1bfa5b3b2` | Raven V2.1.0 `b4f7ff70734c` plus WOZT NetFS, Wi-Fi, release-channel, and file-table changes | V2 kernel ABI 6; raw and hardware-H.264 camera APIs available | Clean local branch; the fork's default `main` is older and is not the ThinClient SDK |
| [wup-thinclient](https://github.com/wozt/wup-thinclient) | `main` `dc13a8fb5e42` | Git submodule pins the V2+NetFS SDK exactly at `3da1bfa5b3b2` | Video, input, audio, microphone, brightness, filesystem, and networking use V2 APIs; camera uplink not yet wired into ThinClient | Clean; V2 build and host tests pass; 142892-byte artifact |
| [wup-stream](https://github.com/wozt/wup-stream) | `main` `6ac48c732c9d` | Builds its pad app against an external V2 libwup; host library is self-built with pinned x264 | Proven 640×480 hardware-H.264 camera uplink and host reconstruction; protocol baseline is `a082db95ba62` | Local source matches origin; untracked developer file `argparse` preserved |
| [wup-stream-server-linux](https://github.com/wozt/wup-stream-server-linux) | `main` `cf0e9eb946e8` | Pins camera-capable wup-stream `a082db95ba62` | FFmpeg decode, 640×480 BGRA preview, and 640×480 YUYV V4L2 output provide the reference implementation | Local source matches origin; Python caches preserved |
| [wup-thinserver](https://github.com/wozt/wup-thinserver) | `main` (audit baseline `232e9500b0f6`) | Camera-capable wup-stream `a082db95ba62`; local direct-connect, mode-control, brightness, capability, and I420 patches are reapplied at build time | V2 ThinClient-compatible desktop stream works; camera receive/preview/announce not yet integrated | Repinned build and six host tests pass; two untracked QEMU screenshots preserved |
| [wup-boy](https://github.com/wozt/wup-boy) | public `main` `03779aa4e91b`; local `feature/net-roms` `a2c082b492dc` | External `LIBWUP`, not revision-pinned; local branch relies on WOZT `/net` | V2 compatibility depends on selecting the V2 SDK; no camera use | Local NetFS branch clean; preserve network-ROM features |
| [capture2cloud](https://github.com/wozt/capture2cloud) | `main` `1fb2dc2bba31` | Isolated host helper pins pre-camera wup-stream `8609916410a2`; no GamePad libwup app | Current video/audio/input role remains wire-compatible with V2 because it never requests camera; repin only if it becomes a camera consumer | Clean; preserve the separate helper to avoid x264 ABI conflicts |

A local unpublished `wup-boy-advance` checkout at
`e7a35936a45d` is already rebuilt for the V2 kernel and uses NetFS ROM
paging. It has no configured remote and no camera use; it is recorded here
but is not presented as a public WOZT repository.

## V1 to V2 findings

V1 and V2 `.wup` binaries are ABI-incompatible. ThinClient has already made
the required source migration and is built against the correct V2 fork. Its
decoder is owned by the V2 kernel through `wup_video_open()`,
`wup_video_frame()`, `wup_video_need_key()`, `wup_video_waiting()`, and
`wup_video_close()`; input timing uses `wup_input_report()` and
`wup_us_since_vblank()`. Audio, microphone, hardware backlight, BSD sockets,
littlefs paths, and NetFS remain compatible at their public APIs.

The WOZT fork preserves `/config/netfs`, `/net`, staged network launches, and
the existing littlefs data. Updating ThinClient to Raven's default branch
directly would discard developer-specific NetFS behavior and is therefore not
an acceptable migration.

## Camera-compatible baseline

The selected reusable baseline is:

- WUP Homebrew V2.1.0 plus WOZT NetFS at `3da1bfa5b3b2`;
- WUP Stream camera merge at `a082db95ba62` for ThinServer's pinned host
  library;
- current ThinClient behavior and protocol extensions retained on top.

The GamePad camera encoder produces 640×480 H.264 bare slice data at about
59 frames/s. Its default rate is 3000 kbit/s, with upstream hardware choices
from 2000 to 6000 kbit/s. The established backward-compatible wire extension
uses hello byte 2 bit 1 to request capture, `C` packets containing at most
1024 camera bytes, and `K` to restart the encoder after loss. The camera is
off until requested and can run beside V2 video decoding.

ThinServer must preserve its existing five-byte hello extension: the camera
request remains in byte 2 while byte 4 continues to report effective input
mode. Its direct endpoint, NAT isolation, brightness, and `T` mode-control
patches are independent and must be reapplied to the new pin.

## Preserved local work

The audit made no changes to reference checkouts. Existing untracked files
remain untouched:

- ThinServer: `docs/qemu-evidence/desktop-live.png` and
  `docs/qemu-evidence/docker-desktop.png`;
- WUP Stream: `argparse`;
- WUP Stream Server Linux: Python `__pycache__` directories.

ThinClient's proven camera sender has now been ported without replacing its
HOME state machine, profiles, direct endpoints, NetFS, or streaming
optimizations. The next increment is the host receiver and its bounded local
preview transport.
