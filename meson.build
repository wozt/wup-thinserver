project('wup-thinserver', 'c',
  version: '0.1.0',
  default_options: ['c_std=c11', 'warning_level=2'])

# WUP ThinServer: headless streaming daemon. No GTK: the desktop session is
# owned by the container entrypoint / native session launcher.

glib = dependency('glib-2.0')
x11 = dependency('x11')
xrandr = dependency('xrandr')
xtst = dependency('xtst')
xext = dependency('xext')
xinerama = dependency('xinerama')
xcb = dependency('xcb')
xcbshm = dependency('xcb-shm')
x11xcb = dependency('x11-xcb')
pulse = dependency('libpulse-simple')
cairo = dependency('cairo')
pangocairo = dependency('pangocairo')
json_glib = dependency('json-glib-1.0')
swscale = dependency('libswscale')
drm = dependency('libdrm')
gbm = dependency('gbm')
egl = dependency('egl')
gles = dependency('glesv2')
threads = dependency('threads')
m_dep = meson.get_compiler('c').find_library('m', required: true)

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
  'src/settings.c',
  'src/x11util.c',
  'src/capture.c',
  'src/capture_kms.c',
  'src/capture_async.c',
  'src/audio.c',
  'src/input.c',
  'src/server.c',
  'src/i18n.c',
  wup_dep[1],
  c_args: ['-DWUP_THIN_VERSION="' + meson.project_version() + '"'],
  include_directories: [include_directories('src')],
  dependencies: [glib, x11, xrandr, xtst, xext, xinerama, json_glib,
                 threads, m_dep, swscale, drm, gbm, egl, gles,
                 xcb, xcbshm, x11xcb, pulse, cairo, pangocairo],
  link_with: wup_dep[0],
  install: true)

install_data('config/settings.example.json',
  install_dir: get_option('datadir') / 'wup-thinserver')
install_data('scripts/onboard-toggle.sh',
  install_dir: get_option('datadir') / 'wup-thinserver')

# --- tests ------------------------------------------------------------------
cap_test = executable('wup-capture-test',
  'tests/capture_test.c',
  'src/settings.c', 'src/x11util.c', 'src/capture.c', 'src/capture_kms.c',
  include_directories: [include_directories('src')],
  dependencies: [glib, x11, xrandr, xtst, xext, xinerama,
                 json_glib, threads, m_dep, swscale, drm, gbm, egl, gles,
                 xcb, xcbshm, x11xcb],
  link_with: wup_dep[0],
  build_by_default: false)

py = find_program('python3')
test('config', py, args: [files('tests/test_config.py')])
test('geometry', py, args: [files('tests/test_geometry.py')])
test('capture-xvfb', find_program('sh'),
  args: [files('tests/run_capture_test.sh')],
  depends: cap_test, timeout: 60)
