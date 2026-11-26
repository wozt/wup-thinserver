# WUP ThinServer Settings

`wup-thinserver-settings` is the GTK3 configuration application installed in
the dedicated ThinServer desktop. The streaming daemon remains GTK-free; use
Meson's `-Dsettings_app=disabled` option for daemon-only builds.

## Implemented

- Complete GamePad button list with HOME clearly reserved for ThinClient.
- Live Desktop/Gamepad mode selector with the effective daemon mode, uinput
  availability, and fallback reason reported by the daemon.
- Disabled, key/shortcut capture, mouse buttons, wheel actions, safe desktop
  shortcuts, function keys, and the Onboard toggle.
- Independent left/right stick modes, directional keys, deadzone,
  sensitivity, and vertical inversion.
- Touch-as-pointer, gyroscope mode/sensitivity, microphone forwarding, and
  physical volume-slider controls.
- A live 4 Hz diagnostic view for connection state, pressed buttons, sticks,
  touch coordinates, and volume. It performs no repaint work per video frame.
- Live validation, application, atomic persistence, reload, and restore
  defaults through the local control socket.

The interface uses a compact borderless WOZT-style window with an integrated
title bar, taskbar identity, eight resize zones, framed sections, square
controls, fixed status/actions, and vertically scrolling content. It is sized
and manually rendered at 854×480; larger ThinServer desktop sizes have more
working space.

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
