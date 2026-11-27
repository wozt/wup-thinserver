#!/usr/bin/env bash
# package-appimage.sh — build wup-thinserver-<version>-<arch>.AppImage.
#
# One AppImage per architecture containing the GTK Settings app (default
# entrypoint) and the streaming daemon (`AppImage daemon`). Runtime shared
# libraries are deployed by linuxdeploy; GTK pixbuf loaders, icons, themes
# data and the MIME database are bundled explicitly.
#
# The full streamed-desktop *session* (Xvfb/XFCE/PulseAudio/Onboard) is NOT
# bundled — the AppImage carries the two appliance binaries. For the
# complete headless appliance use the .deb, the native zip or the systemd
# unit installed by scripts/install-native.sh.
#
# Must run on the target architecture (linuxdeploy is arch-specific):
#   amd64 -> build-native-amd64/install-root
#   arm64 -> build-arm64/install-root   (cross-built tree, bundled on an
#          aarch64 host or CI arm64 runner)
#
# Requires: linuxdeploy (LINUXDEPLOY=/path or PATH), pkg-config, gdk-pixbuf
# tooling, appimagetool runtime (linuxdeploy appimage plugin fetches it).
#
# Usage: scripts/package-appimage.sh [version] [outdir]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${1:-$(git -C "$ROOT" describe --tags --always 2>/dev/null || echo dev)}"
VERSION="${VERSION#v}"
OUTDIR="${2:-$ROOT/build/dist}"

MACHINE="$(uname -m)"
case "$MACHINE" in
    x86_64)  ARCH=amd64; STAGE="$ROOT/build-native-amd64/install-root" ;;
    aarch64) ARCH=arm64; STAGE="$ROOT/build-arm64/install-root" ;;
    *) echo "unsupported host arch for AppImage: $MACHINE" >&2; exit 2 ;;
esac
log() { printf '\033[1;36m[package-appimage]\033[0m %s\n' "$*"; }
[ -x "$STAGE/usr/local/bin/wup-thinserver" ] || {
    echo "missing staged tree at $STAGE — build it first" >&2; exit 1; }

LD="${LINUXDEPLOY:-}"
if [ -z "$LD" ]; then LD="$(command -v linuxdeploy || true)"; fi
[ -n "$LD" ] && [ -e "$LD" ] || {
    echo "linuxdeploy required: LINUXDEPLOY=/path/to/linuxdeploy.AppImage" >&2
    exit 1; }
LD="$(realpath "$LD" 2>/dev/null || command -v "$LD")"

APPDIR_NAME="wup-thinserver-$VERSION-$ARCH.AppDir"
APPDIR="$OUTDIR/$APPDIR_NAME"
rm -rf "$APPDIR"
mkdir -p "$OUTDIR" "$APPDIR/usr/bin" "$APPDIR/usr/share/applications" \
         "$APPDIR/usr/share/icons/hicolor/scalable/apps"

# --- payload: both appliance binaries + helper scripts -------------------------
install -m 755 "$STAGE/usr/local/bin/wup-thinserver" "$APPDIR/usr/bin/"
install -m 755 "$STAGE/usr/local/bin/wup-thinserver-settings" "$APPDIR/usr/bin/"
# Shell/python helpers referenced by the session entrypoint.
for helper in wup-thin-entrypoint onboard-toggle onboard-setup \
              wup-set-password wup-gamepad-devd wup-prepare-native-camera; do
    [ -f "$STAGE/usr/local/bin/$helper" ] &&
        install -m 755 "$STAGE/usr/local/bin/$helper" "$APPDIR/usr/bin/"
done
install -Dm644 "$STAGE/usr/local/share/wup-thinserver/settings.example.json" \
    "$APPDIR/usr/share/wup-thinserver/settings.example.json" 2>/dev/null || \
    { mkdir -p "$APPDIR/usr/share/wup-thinserver"; \
      install -m644 "$STAGE/usr/local/share/wup-thinserver/settings.example.json" \
        "$APPDIR/usr/share/wup-thinserver/"; }
install -Dm644 "$STAGE/usr/local/share/icons/hicolor/scalable/apps/wup-thinserver-settings.svg" \
    "$APPDIR/usr/share/icons/hicolor/scalable/apps/wup-thinserver-settings.svg" || true
install -Dm644 "$ROOT/data/wup-thinserver-settings.svg" \
    "$APPDIR/usr/share/icons/hicolor/scalable/apps/wup-thinserver-settings.svg" 2>/dev/null || true

cat > "$APPDIR/usr/share/applications/wup-thinserver-settings.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=WUP ThinServer Settings
Comment=Settings for the WUP ThinServer streamed desktop
Exec=wup-thinserver-settings
Icon=wup-thinserver-settings
Categories=Settings;Network;
Terminal=false
EOF

# --- GTK runtime data: icons, mime, pixbuf loaders ----------------------------
for theme in Adwaita hicolor; do
    [ -d "/usr/share/icons/$theme" ] || continue
    mkdir -p "$APPDIR/usr/share/icons/$theme"
    cp -a "/usr/share/icons/$theme/." "$APPDIR/usr/share/icons/$theme/"
done
find "$APPDIR/usr/share/icons" -name icon-theme.cache -delete 2>/dev/null || true
[ -d /usr/share/mime ] && { mkdir -p "$APPDIR/usr/share/mime"; cp -a /usr/share/mime/. "$APPDIR/usr/share/mime/"; }

# gdk-pixbuf loaders: deploy each .so via linuxdeploy --library and generate
# a runtime loaders.cache with @APPDIR@ paths substituted by AppRun.
PIXBUF_DIR="$(pkg-config --variable=gdk_pixbuf_moduledir gdk-pixbuf-2.0 2>/dev/null || true)"
PIXBUF_QUERY="$(pkg-config --variable=gdk_pixbuf_query_loaders gdk-pixbuf-2.0 2>/dev/null || true)"
LOADER_ARGS=()
if [ -n "$PIXBUF_DIR" ] && [ -d "$PIXBUF_DIR" ] && [ -n "$PIXBUF_QUERY" ] && [ -x "$PIXBUF_QUERY" ]; then
    shopt -s nullglob
    loaders=("$PIXBUF_DIR"/*.so)
    for loader in "${loaders[@]}"; do LOADER_ARGS+=(--library "$loader"); done
    mkdir -p "$APPDIR/usr/share/wup-thinserver"
    "$PIXBUF_QUERY" "${loaders[@]}" \
        | sed "s|$PIXBUF_DIR/|@APPDIR@/usr/lib/|g" \
        > "$APPDIR/usr/share/wup-thinserver/pixbuf-loaders.cache.in"
fi

# --- custom AppRun: dispatch settings (default) / daemon -----------------------
cat > "$OUTDIR/wup-thinserver-AppRun" <<'LAUNCHER'
#!/bin/sh
set -eu
APPDIR="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
export APPDIR
export LD_LIBRARY_PATH="$APPDIR/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export XDG_DATA_DIRS="$APPDIR/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
export GTK_DATA_PREFIX="$APPDIR/usr"
if [ -f "$APPDIR/usr/share/wup-thinserver/pixbuf-loaders.cache.in" ]; then
    cache="$(mktemp "${TMPDIR:-/tmp}/wup-pixbuf.XXXXXXXX")"
    trap 'rm -f "$cache"' EXIT
    escaped="$(printf '%s' "$APPDIR" | sed 's/[\\&|]/\\&/g')"
    sed "s|@APPDIR@|$escaped|g" \
        "$APPDIR/usr/share/wup-thinserver/pixbuf-loaders.cache.in" > "$cache"
    export GDK_PIXBUF_MODULE_FILE="$cache"
fi
case "${1:-}" in
    daemon|--daemon) shift; exec "$APPDIR/usr/bin/wup-thinserver" "$@" ;;
    *) exec "$APPDIR/usr/bin/wup-thinserver-settings" "$@" ;;
esac
LAUNCHER
chmod 755 "$OUTDIR/wup-thinserver-AppRun"

chmod -R a+rX "$APPDIR"

# --- linuxdeploy: shared libs for both ELF binaries ----------------------------
exclude=()
for pattern in 'libEGL*.so*' 'libGLES*.so*' 'libGL*.so*' 'libgbm.so*' \
    'libdrm*.so*' 'libglapi.so*' 'libLLVM*.so*' 'libvulkan*.so*' \
    'libwayland-*.so*' 'libxcb*.so*' 'libxkbcommon*.so*' \
    'libglib-2.0.so*' 'libgio-2.0.so*' 'libgobject-2.0.so*' \
    'libgmodule-2.0.so*' 'libgthread-2.0.so*' 'libffi.so*' 'libpcre2-*.so*' \
    'libmount.so*' 'libblkid.so*' \
    'libstdc++.so*' 'libgcc_s.so*' 'libzstd.so*' 'libexpat.so*'; do
    exclude+=("--exclude-library=$pattern")
done

OUTPUT="$OUTDIR/wup-thinserver-$VERSION-$ARCH.AppImage"
export APPIMAGE_EXTRACT_AND_RUN=1
# linuxdeploy names the output after the desktop entry and writes it to the
# current directory — run it inside OUTDIR and normalize the name after.
rm -f "$OUTDIR"/*.AppImage
(
    cd "$OUTDIR"
    "$LD" "${exclude[@]}" --appdir "$APPDIR" \
        --executable "$APPDIR/usr/bin/wup-thinserver" \
        --executable "$APPDIR/usr/bin/wup-thinserver-settings" \
        "${LOADER_ARGS[@]}" \
        --custom-apprun "$OUTDIR/wup-thinserver-AppRun" \
        --desktop-file "$APPDIR/usr/share/applications/wup-thinserver-settings.desktop" \
        --output appimage
)

shopt -s nullglob
produced=("$OUTDIR"/*.AppImage)
[ "${#produced[@]}" -eq 1 ] || { ls -la "$OUTDIR"; echo "AppImage not produced" >&2; exit 1; }
mv "${produced[0]}" "$OUTPUT"
log "appimage: $OUTPUT"
sha256sum "$OUTPUT" > "$OUTPUT.sha256"
cat "$OUTPUT.sha256"
