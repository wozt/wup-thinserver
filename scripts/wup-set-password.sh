#!/bin/sh
# wup-set-password: set or change the `wup` account password inside the
# container. Must run as root:
#
#   docker exec -it wup-thinserver wup-set-password
#   docker exec -u root wup-thinserver wup-set-password
#
# Prompts twice (no echo). The password is never printed or logged.
set -eu

if [ "$(id -u)" != "0" ]; then
    echo "wup-set-password: must run as root (docker exec -u root ...)" >&2
    exit 1
fi

printf 'New password for wup: ' >&2
stty -echo 2>/dev/null || true
IFS= read -r p1
printf '\nRepeat password: ' >&2
IFS= read -r p2
stty echo 2>/dev/null || true
printf '\n' >&2

if [ "$p1" != "$p2" ]; then
    echo "wup-set-password: passwords do not match" >&2
    exit 1
fi
if [ -z "$p1" ]; then
    echo "wup-set-password: empty password refused" >&2
    exit 1
fi

printf 'wup:%s\n' "$p1" | chpasswd
unset p1 p2
echo "wup-set-password: password updated for 'wup'"
