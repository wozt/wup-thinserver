project('wup-thinserver', 'c',
  version: '0.1.0',
  default_options: ['c_std=c11', 'warning_level=2'])

# WUP ThinServer: headless streaming daemon. No GTK: the desktop session is
# owned by the container entrypoint / native session launcher.

glib = dependency('glib-2.0')
gio = dependency('gio-2.0')
x11 = dependency('x11')
xrandr = dependency('xrandr')
xtst = dependency('xtst')
xext = dependency('xext')
xfixes = dependency('xfixes')
xinerama = dependency('xinerama')
xcb = dependency('xcb')
xcbshm = dependency('xcb-shm')
x11xcb = dependency('x11-xcb')
pulse = dependency('libpulse-simple')
cairo = dependency('cairo')
pangocairo = dependency('pangocairo')
json_glib = dependency('json-glib-1.0')
swscale = dependency('libswscale')
avcodec = dependency('libavcodec')
avutil = dependency('libavutil')
drm = dependency('libdrm')
gbm = dependency('gbm')
egl = dependency('egl')
gles = dependency('glesv2')
threads = dependency('threads')
m_dep = meson.get_compiler('c').find_library('m', required: true)
gtk3 = dependency('gtk+-3.0', required: get_option('settings_app'))
gdkx11 = dependency('gdk-x11-3.0', required: gtk3.found())
sdl2 = dependency('sdl2', required: false)

# libwupstream (WUP Stream protocol + patched x264), pinned upstream source
# built inside the Meson build directory. See docs/UPSTREAM.md.
wup_python = find_program('python3')
wup_dep = custom_target('wupstream-embedded',
  output: ['libwupstream.a', 'wupstream.h'],
  command: [wup_python, files('tools/build_wupstream_dep.py'),
            '@OUTPUT0@', '@OUTPUT1@'],
  build_by_default: true)

executable('wup-thinserver',
  'src/main.c',
  'src/config.c',
  'src/control.c',
  'src/settings.c',
  'src/x11util.c',
  'src/capture.c',
  'src/capture_kms.c',
  'src/capture_async.c',
  'src/audio.c',
  'src/camera.c',
  'src/input.c',
  'src/server.c',
  'src/i18n.c',
  wup_dep[1],
  c_args: ['-DWUP_THIN_VERSION="' + meson.project_version() + '"'],
  include_directories: [include_directories('src')],
  dependencies: [glib, gio, x11, xrandr, xtst, xext, xfixes, xinerama, json_glib,
                 threads, m_dep, swscale, avcodec, avutil, drm, gbm, egl, gles,
                 xcb, xcbshm, x11xcb, pulse, cairo, pangocairo],
  link_with: wup_dep[0],
  install: true)

if gtk3.found()
  executable('wup-thinserver-settings',
    'src/settings_app.c',
    'src/settings.c',
    wup_dep[1],
    include_directories: [include_directories('src')],
    dependencies: [gtk3, gdkx11, gio, json_glib, x11],
    install: true)
  install_data('data/wup-thinserver-settings.desktop',
    install_dir: get_option('datadir') / 'applications')
  install_data('data/wup-thinserver-settings.svg',
    install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps')
endif

# Privileged KMS capture helper: spawned by wup-thinserver with inherited
# socket/fd pair; exports the scanout framebuffer as DMA-BUFs. Installed
# setcap cap_sys_admin=ep by the installers (see scripts/install-native.sh).
executable('wup-kms-helper',
  'src/kms_helper.c',
  include_directories: [include_directories('src')],
  dependencies: [drm],
  install: true,
  install_dir: get_option('libexecdir') / 'wupstreamd')

install_data('config/settings.example.json',
  install_dir: get_option('datadir') / 'wup-thinserver')
install_data('scripts/onboard-toggle.sh',
  install_dir: get_option('datadir') / 'wup-thinserver')
install_data('scripts/install-desktop-shortcut.sh',
  install_dir: get_option('bindir'),
  rename: 'wup-install-desktop-shortcut',
  install_mode: 'rwxr-xr-x')

# Session helpers shared by the Docker image and the native installer:
# everything lands under $(bindir) so `meson install` produces a complete
# /usr/local tree for both deployment paths.
foreach h : [
    ['provisioning/session/entrypoint.sh', 'wup-thin-entrypoint'],
    ['scripts/onboard-toggle.sh', 'onboard-toggle'],
    ['scripts/onboard-setup.sh', 'onboard-setup'],
    ['scripts/migrate-settings.py', 'wup-migrate-settings'],
    ['scripts/wup-set-password.sh', 'wup-set-password'],
    ['scripts/gamepad-devd.sh', 'wup-gamepad-devd'],
    ['scripts/prepare-native-camera.sh', 'wup-prepare-native-camera'],
  ]
  install_data(h[0],
    install_dir: get_option('bindir'),
    rename: h[1],
    install_mode: 'rwxr-xr-x')
endforeach

# --- tests ------------------------------------------------------------------
cap_test = executable('wup-capture-test',
  'tests/capture_test.c',
  'src/settings.c', 'src/x11util.c', 'src/capture.c', 'src/capture_kms.c',
  include_directories: [include_directories('src')],
  dependencies: [glib, x11, xrandr, xtst, xext, xfixes, xinerama,
                 json_glib, threads, m_dep, swscale, drm, gbm, egl, gles,
                 xcb, xcbshm, x11xcb],
  link_with: wup_dep[0],
  build_by_default: false)

py = find_program('python3')
test('config', py, args: [files('tests/test_config.py')])
test('migration', py, args: [files('tests/test_migration.py')])
test('geometry', py, args: [files('tests/test_geometry.py')])
test('control-ipc', py, args: [files('tests/test_control.py')], timeout: 30)
test('encoded-cursor', py, args: [files('tests/test_encoded_cursor.py')],
  timeout: 40, is_parallel: false)
test('capture-xvfb', find_program('sh'),
  args: [files('tests/run_capture_test.sh')],
  depends: cap_test, timeout: 60)

camera_test = executable('wup-camera-test',
  'tests/camera_test.c', 'src/camera.c', wup_dep[1],
  c_args: ['-DWUP_CAMERA_TEST'],
  include_directories: [include_directories('src')],
  dependencies: [threads, swscale, avcodec, avutil],
  build_by_default: false)
test('camera-worker', camera_test, depends: camera_test, timeout: 20)

if sdl2.found()
  gamepad_test = executable('wup-gamepad-test',
    'tests/input_gamepad_test.c',
    'src/settings.c', 'src/x11util.c', 'src/input.c',
    wup_dep[1],
    include_directories: [include_directories('src')],
    dependencies: [glib, json_glib, x11, xrandr, xtst, xext, xinerama,
                   sdl2, m_dep],
    build_by_default: false)
  test('gamepad-evdev-sdl', gamepad_test, depends: gamepad_test, timeout: 20)
endif
