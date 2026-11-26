# Protocol

WUP ThinServer speaks the **WUP Stream protocol** unchanged — the same wire
format implemented by `libwupstream` (host side, in this daemon) and the pad
receiver (`wup-thinclient`, derived from `wup-stream/pad`). No RTP/RTSP/VNC/
RDP is used.

All integers little-endian. The GamePad side binds UDP **50000**; discovery
uses UDP **50001** broadcast.

## Session establishment

1. Client (idle) broadcasts `B 0x01 0x00 0x00` to `255.255.255.255:50001`
   every second.
2. Daemon (`wupstream_open`) listens on 50001 (SO_REUSEADDR/REUSEPORT) for a
   `B` packet, or connects directly to `pad_ip:50000` when configured.
3. Daemon sends `H` hellos every 250 ms: `'H' ver flags mic light`. `flags`
   bit0 = request microphone; `light` = backlight level 1..6 (bit6 = off,
   bit7 = keep).
4. Client answers `I` input packets (below). Once the refresh timing is
   locked, the daemon starts encoding.

## Daemon → client

| Tag | Layout | Meaning |
|---|---|---|
| `H` | `H ver flags mic light` (4 B) | hello / keepalive |
| `V` | `'V' flags(1=IDR) 0 0, u32 frame, u32 offset, u32 total, ≤1400 B` | one H.264 frame split in 1400-byte fragments of **raw CABAC slice data** (no start codes, no NAL headers, emulation-prevention bytes stripped). `flags&1` marks an IDR. |
| `A` | `'A' 0 0 0, u32 seq, PCM` | 48 kHz s16le **stereo** audio; payload a multiple of 16 bytes |
| `R` | `'R' on 0 0` | rumble motor |
| `Q` | `'Q'` | daemon done — client returns to its menu |

Video frames are paced to the client screen's 59.94 Hz refresh (measured
from `I` packets' clock ticks). The encoder keeps a cushion of frames on
the client; the client NACKs missing fragments.

## Client → daemon

| Tag | Layout | Meaning |
|---|---|---|
| `I` | `'I' flags u16 ticks u8 backlight 0 0 0, 128-byte report` | input report @ ~200 Hz. `flags` bit0 = request keyframe; bits1-3 = decoder queue depth. `ticks` ≈ µs since last screen refresh. Report: buttons/sticks/touch/IMU/volume/battery (see upstream `pad/stream`). |
| `N` | `'N' count 0 0, count × {u32 frame, u16 packet (0xffff=all), u16 0}` | request retransmission of lost video fragments |
| `M` | `'M' 0 0 0, u32 sample, PCM mono 48 kHz` | client microphone (when enabled by `H` flags bit0) |

## Encoding constraints (GamePad hardware decoder)

- H.264 **High** profile, CABAC, 8-bit 4:2:0, 854×480, ~60 fps.
- x264 patched (`x264-drh-mode.patch`): **no slice headers, no planar
  prediction** — the pad feeds the bitstream ring directly.
- Encoder settings pinned by `libwupstream`: `drh-mode=1 ref=1 bframes=0
  cabac=1 constrained-intra=1 weightp=0 8x8dct=0 scenecut=0 keyint=infinite
  slices=1 threads=1 aud=0`, preset `veryfast`/`zerolatency`, CRF quality
  capped by `kbps` (VBV = kbps/60 per frame, 3-frame borrow).
- Alignment: encoded size is fixed **854×480**; the desktop resolution is
  scaled server-side. Do not change `WUPSTREAM_W/H` without a matching pad
  decoder change.

## Loss / recovery

- Client NACKs missing fragments (`N`); daemon resends from a ring of the
  last 8 frames.
- `flags bit0` in `I` requests an IDR (decoder resync).
- Client watchdog: no decoder progress for 60 ms → demand keyframe.
- Daemon: pad silent >2 s → session ends; `auto_reconnect` waits for it.

## Audio

48 kHz, s16le. PC→pad stereo interleaved; pad→PC mono. The daemon paces
audio by wall clock, decoupled from video.
