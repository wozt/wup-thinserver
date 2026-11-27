#!/usr/bin/env bash
# package-deb.sh — build wup-thinserver_<version>_<arch>.deb.
#
# The package ships the staged /usr/local tree (daemon + GTK Settings app +
# session helpers), the systemd unit and a postinst that performs the same
# first-install steps as scripts/install-native.sh: wup account, baseline
# passwords, settings file and service enable. APT dependencies are
# declared from provisioning/packages/*.txt so `apt install ./file.deb`
# pulls the whole desktop session.
#
# Reuses the staged trees produced by package-native.sh / build-arm64.sh:
#   amd64 -> build-native-amd64/install-root
#   arm64 -> build-arm64/install-root
#
# Usage: scripts/package-deb.sh <amd64|arm64> [version] [outdir]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARCH="${1:-amd64}"
VERSION="${2:-$(git -C "$ROOT" describe --tags --always 2>/dev/null || echo dev)}"
VERSION="${VERSION#v}"
OUTDIR="${3:-$ROOT/build/dist}"

case "$ARCH" in amd64|arm64) ;; *) echo "usage: $0 <amd64|arm64> [version] [outdir]" >&2; exit 2;; esac

log() { printf '\033[1;36m[package-deb]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[package-deb] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

case "$ARCH" in
    amd64) STAGE="$ROOT/build-native-amd64/install-root" ;;
    arm64)
        if [ "$(uname -m)" = aarch64 ]; then
            STAGE="$ROOT/build-native-arm64/install-root"
        else
            STAGE="$ROOT/build-arm64/install-root"
        fi
        ;;
esac
[ -x "$STAGE/usr/local/bin/wup-thinserver" ] ||
    die "missing staged tree at $STAGE — run package-native.sh or build-arm64.sh first"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
PKGROOT="$WORK/pkg"
DEBIAN="$PKGROOT/DEBIAN"
mkdir -p "$DEBIAN" "$PKGROOT/usr/lib/systemd/system" \
         "$PKGROOT/usr/share/wup-thinserver"

# --- payload -----------------------------------------------------------------
cp -a "$STAGE/usr/local" "$PKGROOT/usr/"
install -m 644 "$ROOT/packaging/systemd/wup-thinserver.service" \
    "$PKGROOT/usr/lib/systemd/system/wup-thinserver.service"

# Depends straight from the shared package lists (no -dev packages: those
# are build-time only and already absent from runtime.txt/desktop.txt).
DEPS="$(grep -hEv '^\s*(#|$)' \
    "$ROOT/provisioning/packages/runtime.txt" \
    "$ROOT/provisioning/packages/desktop.txt" \
    | sort -u | sed 's/$/,/' | tr -d '\n' | sed 's/,$//; s/,/, /g')"
[ -n "$DEPS" ] || die "empty dependency list"

cat > "$DEBIAN/control" <<EOF
Package: wup-thinserver
Version: $VERSION
Section: net
Priority: optional
Architecture: $ARCH
Depends: $DEPS
Maintainer: WUP ThinServer <noreply@localhost>
Description: Headless streamed desktop appliance for the Wii U GamePad
 WUP ThinServer runs a virtual XFCE desktop (Xvfb), a PulseAudio null sink,
 Onboard and the wup-thinserver streaming daemon as an unprivileged user.
 No physical display or login manager is required.
EOF

cat > "$DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
case "$1" in
    configure)
        if ! id wup >/dev/null 2>&1; then
            useradd -m -s /bin/bash wup
            usermod -aG sudo wup
            mkdir -p /home/wup/.config/wup-thinserver
            cp /usr/local/share/wup-thinserver/settings.example.json \
               /home/wup/.config/wup-thinserver/settings.json 2>/dev/null || true
            chown -R wup:wup /home/wup/.config/wup-thinserver
            printf 'wup:wup\nroot:wup\n' | chpasswd
        fi
        mkdir -p /tmp/pulse-runtime /tmp/xdg-runtime
        chmod 700 /tmp/pulse-runtime /tmp/xdg-runtime
        chown wup:wup /tmp/pulse-runtime /tmp/xdg-runtime 2>/dev/null || true
        systemctl daemon-reload 2>/dev/null || true
        systemctl enable wup-thinserver.service 2>/dev/null || true
        if [ -d /run/systemd/system ]; then
            systemctl start wup-thinserver.service 2>/dev/null || true
        fi
        ;;
esac
exit 0
EOF
chmod 0755 "$DEBIAN/postinst"

# prerm: stop the service before file removal on upgrade/purge.
cat > "$DEBIAN/prerm" <<'EOF'
#!/bin/sh
set -e
case "$1" in
    remove|deconfigure)
        systemctl stop wup-thinserver.service 2>/dev/null || true
        systemctl disable wup-thinserver.service 2>/dev/null || true
        ;;
esac
exit 0
EOF
chmod 0755 "$DEBIAN/prerm"

mkdir -p "$OUTDIR"
DEB="$OUTDIR/wup-thinserver_${VERSION}_${ARCH}.deb"
rm -f "$DEB"
dpkg-deb --root-owner-group --build "$PKGROOT" "$DEB" >/dev/null
log "package: $DEB"
sha256sum "$DEB" > "$DEB.sha256"
cat "$DEB.sha256"
