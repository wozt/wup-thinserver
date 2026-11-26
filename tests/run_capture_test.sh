#!/bin/sh
# Run the capture test against a private Xvfb display.
set -eu
cd "$(dirname "$0")/.."
DISP=:${WUP_TEST_DISPLAY:-77}
XVFB_PID=""
WINDOW_PID=""
cleanup() {
    [ -n "$WINDOW_PID" ] && kill "$WINDOW_PID" 2>/dev/null || true
    [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null || true
}
trap cleanup EXIT

Xvfb "$DISP" -screen 0 1280x720x24 >/dev/null 2>&1 &
XVFB_PID=$!
for i in $(seq 1 50); do
    DISPLAY=$DISP xdpyinfo >/dev/null 2>&1 && break
    sleep 0.1
done
# A real window gives the root framebuffer non-black pixels.
DISPLAY=$DISP xlogo >/dev/null 2>&1 &
WINDOW_PID=$!
sleep 0.5

DISPLAY=$DISP ./build/wup-capture-test
