#!/bin/sh
# Install the managed launcher into the user's localized XDG desktop.
# Existing user-created launchers are never replaced.
set -eu

source_file="${WUP_SETTINGS_DESKTOP_FILE:-/usr/local/share/applications/wup-thinserver-settings.desktop}"
[ -r "$source_file" ] || exit 0

desktop_dir=""
if command -v xdg-user-dir >/dev/null 2>&1; then
    desktop_dir=$(xdg-user-dir DESKTOP 2>/dev/null || true)
fi
if [ -z "$desktop_dir" ] || [ "$desktop_dir" = "$HOME" ]; then
    desktop_dir="$HOME/Desktop"
fi

target="$desktop_dir/wup-thinserver-settings.desktop"
mkdir -p "$desktop_dir"
if [ ! -e "$target" ]; then
    install -m 0755 "$source_file" "$target"
    echo "wup-install-desktop-shortcut: installed $target"
fi
chmod u+x "$target" 2>/dev/null || true
if command -v gio >/dev/null 2>&1; then
    gio set "$target" metadata::trusted true >/dev/null 2>&1 || true
fi
