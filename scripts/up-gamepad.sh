#!/bin/sh
# Start WUP ThinServer with GamePad (uinput) support enabled.
# Equivalent to:
#   docker compose -f docker-compose.yml -f docker-compose.gamepad.yml up -d --build "$@"
# Extra arguments are forwarded to `docker compose up` (e.g. --build, -d).
set -eu
cd "$(dirname "$0")"
exec docker compose -f docker-compose.yml -f docker-compose.gamepad.yml up -d --build "$@"
