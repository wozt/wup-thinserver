# WUP ThinServer Settings

`wup-thinserver-settings` is the GTK3 configuration application installed in
the dedicated ThinServer desktop. The streaming daemon remains GTK-free; use
Meson's `-Dsettings_app=disabled` option for daemon-only builds.

## Implemented

- Single-page dashboard (no tabs): compact rows in one vertically scrolling
  area. Analog sticks, Display & audio, Touch & motion, and Camera share the
  responsive secondary row; Camera is the fourth section, immediately after
  Touch & motion at 1280×720.
- Complete GamePad button list — HOME included and editable like every
  other control; the ThinClient reserves only its local shortcut
  combinations (5 s hold, HOME+X, HOME+Up/Down, HOME+B), never the tap.
- Live Desktop/Gamepad mode selector reflecting the daemon's *effective*
  mode, uinput availability, and fallback reason — including switches
  initiated from the GamePad (HOME+X) or `'T'` packets.
- Disabled, key/shortcut capture, mouse buttons, wheel actions, safe desktop
  shortcuts, function keys, and the Onboard toggle.
- Independent left/right stick modes, directional keys, deadzone,
  sensitivity, and vertical inversion.
- Hardware screen-brightness slider (Auto + discrete levels 1–6) applied
  live through `set_pad_brightness` without restarting stream, daemon or
  container; the live monitor shows the pad's reported backlight level.
- Touch-as-pointer, gyroscope mode/sensitivity, microphone forwarding, and
  physical volume-slider controls.
- Adjacent Camera Announce and Preview controls, dedicated V4L2 node field,
  useful connection/error status, and an embedded 4:3 preview. The preview
  reads the daemon's protected one-slot mmap at up to 15 fps and performs no
  refresh while the section/application is hidden.
- A live 4 Hz diagnostic view for connection state, pressed buttons, sticks,
  touch coordinates, volume, and backlight. It performs no repaint work per
  video frame.
- Live validation, application, atomic persistence, reload, and restore
  defaults through the local control socket.

The interface uses a compact borderless WOZT-style window with an integrated
title bar, taskbar identity, eight resize zones, framed sections, square
controls, fixed status/actions, and a slightly larger base font. The default
size is the window manager's actual work area (`_NET_WORKAREA`), so the XFCE
taskbar stays visible; smaller windows scroll vertically and may scroll
horizontally rather than clip.

## Desktop integration

Meson installs the executable, icon, and Freedesktop launcher. The desktop
entry is available in the XFCE Settings menu. On every session start,
`wup-install-desktop-shortcut` resolves the localized XDG desktop directory
and installs the launcher only when it is absent. It marks the launcher
executable and asks GIO to trust it. Existing customized launchers are not
overwritten, so persistent Docker home volumes receive the shortcut safely.

## Known limitations

The first working version deliberately keeps the editor aligned with fields
that the daemon already implements. The following requested items are
**planned**, not presented as working:

- named custom/browser profiles and import/export;
- separate touch gesture thresholds and long-press duration;
- Onboard layout/docking editing from this application;
- response curves and separate pointer/scroll sensitivity;
- the seven live JSON themes and JSON language switching from the complete
  WOZT GTK standard;
- a drawn controller silhouette (the current monitor is textual).

Arbitrary shell commands remain an advanced JSON-file feature. The graphical
application intentionally cannot create them.
