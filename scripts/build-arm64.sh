#!/usr/bin/env bash
# build-arm64.sh — cross-compile wup-thinserver for Debian 13 arm64.
#
# Toolchain: gcc-aarch64-linux-gnu + arm64 -dev packages (see
# provisioning/packages/build-arm64.txt). pkg-config resolves arm64
# .pc files through PKG_CONFIG_LIBDIR.
#
# Vendored builds need plain `cc`/`uname` to mean "aarch64": a shim bin
# dir is prepended to PATH (uname reports aarch64 so x264's configure
# does not pick x86 assembly).
#
# Usage: scripts/build-arm64.sh [builddir]   (default build-arm64)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="${1:-$ROOT/build-arm64}"
SHIM="$(mktemp -d)"
trap 'rm -rf "$SHIM"' EXIT

cat > "$SHIM/cc" <<'EOF'
#!/bin/sh
exec aarch64-linux-gnu-gcc "$@"
EOF
cat > "$SHIM/c++" <<'EOF'
#!/bin/sh
exec aarch64-linux-gnu-g++ "$@"
EOF
cat > "$SHIM/uname" <<'EOF'
#!/bin/sh
if [ "$1" = "-m" ] || [ "$1" = "--machine" ]; then
    echo aarch64; exit 0
fi
exec /usr/bin/uname "$@"
EOF
chmod +x "$SHIM"/{cc,c++,uname}

export PATH="$SHIM:$PATH"
export CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++ \
       AR=aarch64-linux-gnu-ar STRIP=aarch64-linux-gnu-strip
export PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig
export PKG_CONFIG_PATH=/usr/lib/aarch64-linux-gnu/pkgconfig

cd "$ROOT"
meson setup "$B" --cross-file packaging/cross/aarch64-linux-gnu.ini \
    --buildtype=release -Dsettings_app=enabled
meson compile -C "$B" -j"$(nproc)"
meson install -C "$B" --destdir "$B/install-root"
echo "== binaries =="
file "$B"/wup-thinserver "$B"/wup-thinserver-settings | sed 's/,.*ARM/aarch64/'
