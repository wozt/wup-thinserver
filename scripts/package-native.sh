#!/usr/bin/env bash
# package-native.sh — build the native Linux install bundle.
#
# Produces wup-thinserver-<version>-<arch>.zip containing the staged
# /usr/local tree (daemon + GTK Settings app + session helpers), the
# shared installer, package lists and the systemd unit. On the target PC:
#
#   unzip wup-thinserver-<version>-<arch>.zip
#   cd wup-thinserver-<version>-<arch>
#   sudo ./install.sh
#
# install.sh runs scripts/install-native.sh --prebuilt, which installs the
# APT dependencies, creates the wup account and enables the service.
#
# Usage: scripts/package-native.sh <amd64|arm64> [version] [outdir]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARCH="${1:-amd64}"
VERSION="${2:-$(git -C "$ROOT" describe --tags --always 2>/dev/null || echo dev)}"
VERSION="${VERSION#v}"
OUTDIR="${3:-$ROOT/build/dist}"

case "$ARCH" in amd64|arm64) ;; *) echo "usage: $0 <amd64|arm64> [version] [outdir]" >&2; exit 2;; esac

log() { printf '\033[1;36m[package-native]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[package-native] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
PKG="$STAGE/wup-thinserver-$VERSION-$ARCH"
mkdir -p "$PKG/source/scripts" "$PKG/source/provisioning/packages" \
         "$PKG/source/packaging/systemd" "$PKG/prebuilt"

# --- staged /usr/local tree -------------------------------------------------
if [ "$ARCH" = arm64 ]; then
    log "cross-building arm64 payload"
    "$ROOT/scripts/build-arm64.sh" "$ROOT/build-arm64" >/dev/null
    cp -a "$ROOT/build-arm64/install-root/." "$PKG/prebuilt/"
else
    log "building amd64 payload"
    B="$ROOT/build-native-amd64"
    if [ ! -d "$B" ]; then
        meson setup "$B" --buildtype=release -Dsettings_app=enabled
    fi
    meson compile -C "$B" -j"$(nproc)"
    rm -rf "$B/install-root"
    meson install -C "$B" --destdir "$B/install-root"
    cp -a "$B/install-root/." "$PKG/prebuilt/"
fi
[ -x "$PKG/prebuilt/usr/local/bin/wup-thinserver" ] ||
    die "staged tree lacks wup-thinserver"
[ -x "$PKG/prebuilt/usr/local/bin/wup-thinserver-settings" ] ||
    die "staged tree lacks wup-thinserver-settings"

# --- installer + data ---------------------------------------------------------
cp "$ROOT/scripts/install-native.sh" "$PKG/source/scripts/"
cp "$ROOT/provisioning/packages/runtime.txt" \
   "$ROOT/provisioning/packages/desktop.txt" \
   "$PKG/source/provisioning/packages/"
cp "$ROOT/packaging/systemd/wup-thinserver.service" \
   "$PKG/source/packaging/systemd/"
chmod 0755 "$PKG/source/scripts/install-native.sh"

cat > "$PKG/install.sh" <<'EOF'
#!/usr/bin/env bash
# WUP ThinServer native installer — see README.md in this bundle.
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
exec bash "$DIR/source/scripts/install-native.sh" \
    --prebuilt "$DIR/prebuilt" "$@"
EOF
chmod 0755 "$PKG/install.sh"

cat > "$PKG/README.md" <<EOF
# WUP ThinServer $VERSION ($ARCH)

Headless streamed desktop appliance for the Wii U GamePad ThinClient.

## Install

\`\`\`sh
sudo ./install.sh
\`\`\`

Requires a Debian 13 (trixie) systemd system on $ARCH with network access —
the installer pulls the desktop session packages via APT. Useful options:

- \`--desktop 1280x720\`   virtual display resolution
- \`--port 50004\`         dedicated UDP listen port
- \`--password\`           choose the \`wup\` password (default wup/wup, sudo)
- \`--uninstall\`          remove the installation

The service \`wup-thinserver.service\` starts at boot: Xvfb + XFCE +
PulseAudio null sink + Onboard + the streaming daemon, all as the
unprivileged \`wup\` user. No physical display or login manager needed.
EOF

# --- zip -----------------------------------------------------------------------
mkdir -p "$OUTDIR"
ZIP="$OUTDIR/wup-thinserver-$VERSION-$ARCH.zip"
rm -f "$ZIP"
python3 - "$PKG" "$ZIP" <<'PY'
import os, sys, zipfile
src, dst = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as z:
    for base, dirs, files in os.walk(src):
        dirs.sort()
        for name in sorted(files):
            path = os.path.join(base, name)
            z.write(path, os.path.relpath(path, os.path.dirname(src)))
PY
log "bundle: $ZIP"
sha256sum "$ZIP" > "$ZIP.sha256"
cat "$ZIP.sha256"
