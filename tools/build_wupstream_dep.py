#!/usr/bin/env python3
"""Build pinned WUP Stream (and patched x264) into Meson's build directory.

Invoked by Meson: build_wupstream_dep.py OUT_LIB_A OUT_HEADER_H
No sibling /home/.../wup-stream installation is used, or required.
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

UPSTREAM_URL = "https://github.com/wozt/wup-stream.git"
UPSTREAM_COMMIT = "e1b04305ca123e3f6980d60ef302ada9c19ce127"


def run(command, *, cwd=None):
    print("+ " + " ".join(map(str, command)), flush=True)
    env = os.environ.copy()
    env["GIT_TERMINAL_PROMPT"] = "0"
    subprocess.run([str(arg) for arg in command], cwd=cwd, env=env, check=True)


def git_output(repo, *args):
    return subprocess.check_output(
        ["git", "-C", str(repo), *args], text=True, stderr=subprocess.DEVNULL
    ).strip()


def prepare_source(source):
    source.parent.mkdir(parents=True, exist_ok=True)
    if source.exists() and not (source / ".git").is_dir():
        raise RuntimeError(f"Refusing to overwrite non-Git dependency cache: {source}")
    if not source.exists():
        source.mkdir()
        run(["git", "init", "-q", source])
    try:
        current = git_output(source, "rev-parse", "HEAD")
    except subprocess.CalledProcessError:
        current = None
    if current != UPSTREAM_COMMIT:
        print(f"Fetching pinned wozt/wup-stream revision {UPSTREAM_COMMIT[:12]}",
              flush=True)
        run(["git", "-C", source, "fetch", "--depth", "1",
             UPSTREAM_URL, UPSTREAM_COMMIT])
        run(["git", "-C", source, "checkout", "--force", "--detach", "FETCH_HEAD"])
    if git_output(source, "rev-parse", "HEAD") != UPSTREAM_COMMIT:
        raise RuntimeError("Fetched upstream commit differs from the pin")
    for filename in ("host/build.sh", "host/wupstream.h"):
        if not (source / filename).is_file():
            raise RuntimeError(f"Missing pinned upstream file: {filename}")


def extend_i420(source):
    header = source / "host/wupstream.h"
    code = source / "host/wupstream.c"
    declaration = "int wupstream_frame_i420(wupstream *w, const uint8_t *pixels);"
    implementation = r"""
int wupstream_frame_i420(wupstream *w, const uint8_t *pixels) {
    if (!pixels) return -1;
    pthread_mutex_lock(&w->mu);
    while (w->fresh && !w->gone) pthread_cond_wait(&w->cv, &w->mu);
    if (w->gone) { pthread_mutex_unlock(&w->mu); return -1; }
    const uint8_t *src = pixels;
    for (int i = 0; i < 3; ++i) {
        int width = i ? WUPSTREAM_W / 2 : WUPSTREAM_W;
        int height = i ? WUPSTREAM_H / 2 : WUPSTREAM_H;
        for (int y = 0; y < height; ++y)
            memcpy(w->pic.img.plane[i] + y * w->pic.img.i_stride[i],
                   src + y * width, width);
        src += width * height;
    }
    w->fresh = w->have_pic = 1;
    while (w->fresh && !w->gone) pthread_cond_wait(&w->cv, &w->mu);
    int r = w->gone ? -1 : 0;
    pthread_mutex_unlock(&w->mu);
    return r;
}
"""
    for path, anchor, addition in (
        (header, "int wupstream_audio(", declaration + "\n"),
        (code, "int wupstream_audio(", implementation + "\n"),
    ):
        text = path.read_text()
        if addition not in text:
            if text.count(anchor) != 1:
                raise RuntimeError(f"Unexpected pinned source: {path}")
            path.write_text(text.replace(anchor, addition + anchor, 1))


def extend_brightness_commands(source):
    code = source / "host/wupstream.c"
    old = "w->light = level < 1 ? 0x40 : level | (keep ? 0x80 : 0);"
    new = "w->light = (level < 1 ? 0x40 : level | (keep ? 0x80 : 0)) | ((w->light ^ 0x20) & 0x20);"
    text = code.read_text()
    if new not in text:
        if text.count(old) != 1:
            raise RuntimeError(f"Unexpected pinned backlight implementation: {code}")
        code.write_text(text.replace(old, new, 1))


def extend_control_packets(source):
    """WUP_THIN_PROTOCOL: live input-mode control between pad and daemon.

    pad -> server 'T' reqid mode 0     (4 bytes; mode: 0 gamepad, 1 desktop,
                                        255 toggle; reqid echoes in the ack)
    server -> pad 'T' reqid status mode (status: 0 applied, 1 rejected,
                                        2 invalid; mode: effective mode)
    server -> pad 'H' is extended to 5 bytes: byte 4 is the daemon's current
    effective input mode (0/1, 255 unknown until the input context exists).
    Older peers ignore both extensions: unknown tags are dropped and the
    5th hello byte is never read by clients that only parse 4 bytes.
    """
    header = source / "host/wupstream.h"
    code = source / "host/wupstream.c"

    decl_anchor = "int wupstream_input(wupstream *w, wupstream_input_t *in);"
    decls = decl_anchor + """
void wupstream_set_input_mode(wupstream *w, int mode);   /* effective mode in every 'H' (0/1, 255 unknown) */
int wupstream_take_control(wupstream *w, int *reqid, int *mode);  /* 1: a 'T' request is pending */
void wupstream_control_reply(wupstream *w, int reqid, int status, int mode);"""
    text = header.read_text()
    if decls not in text:
        if text.count(decl_anchor) != 1:
            raise RuntimeError(f"Unexpected pinned header: {header}")
        header.write_text(text.replace(decl_anchor, decls, 1))

    # struct fields: pending 'T' request + the mode advertised in 'H'
    field_anchor = "int16_t micbuf[48000]; int mic_w, mic_n;"
    fields = field_anchor + "\n    int input_mode, ctrl_pending, ctrl_reqid, ctrl_mode;"
    text = code.read_text()
    if fields not in text:
        if text.count(field_anchor) != 1:
            raise RuntimeError(f"Unexpected pinned source: {code}")
        text = text.replace(field_anchor, fields, 1)

    # calloc zeroes everything; 0 would falsely mean "gamepad" in 'H' byte 4
    open_anchor = "wupstream *w = calloc(1, sizeof *w);"
    if open_anchor in text and "w->input_mode = 0xff;" not in text:
        if text.count(open_anchor) != 1:
            raise RuntimeError(f"Unexpected pinned source: {code}")
        text = text.replace(open_anchor, open_anchor + "\n    w->input_mode = 0xff;", 1)

    # 'T' intake inside pump(): last request wins (mode is explicit, so a
    # duplicated packet can never toggle the mode twice)
    intake_anchor = "        if (b[0] == 'I' && n == 136) {"
    intake = ("        if (b[0] == 'T' && n >= 4) { w->ctrl_reqid = b[1];"
              " w->ctrl_mode = b[2]; w->ctrl_pending = 1; }\n" + intake_anchor)
    if intake not in text:
        if text.count(intake_anchor) != 1:
            raise RuntimeError(f"Unexpected pinned source: {code}")
        text = text.replace(intake_anchor, intake, 1)

    # hello: advertise the effective input mode as a 5th byte
    hello_old = "char h[4] = { 'H', 1, (char)w->mic, (char)w->light }; put(w, h, 4);"
    hello_new = ("char h[5] = { 'H', 1, (char)w->mic, (char)w->light,"
                 " (char)w->input_mode }; put(w, h, 5);")
    if hello_new not in text:
        if text.count(hello_old) != 1:
            raise RuntimeError(f"Unexpected pinned hello: {code}")
        text = text.replace(hello_old, hello_new, 1)

    # public API next to wupstream_close
    api_anchor = "void wupstream_close(wupstream *w)"
    api = """void wupstream_set_input_mode(wupstream *w, int mode) {
    pthread_mutex_lock(&w->mu); w->input_mode = mode & 0xff; w->last_hello = 0; pthread_mutex_unlock(&w->mu);
}
int wupstream_take_control(wupstream *w, int *reqid, int *mode) {
    pthread_mutex_lock(&w->mu);
    int ok = w->ctrl_pending; w->ctrl_pending = 0;
    if (ok) { *reqid = w->ctrl_reqid; *mode = w->ctrl_mode; }
    pthread_mutex_unlock(&w->mu);
    return ok;
}
void wupstream_control_reply(wupstream *w, int reqid, int status, int mode) {
    uint8_t p[4] = { 'T', (uint8_t)reqid, (uint8_t)status, (uint8_t)mode };
    pthread_mutex_lock(&w->mu); put(w, p, 4); pthread_mutex_unlock(&w->mu);
}

""" + api_anchor
    if "wupstream_take_control(wupstream *w, int *reqid" not in text:
        if text.count(api_anchor) != 1:
            raise RuntimeError(f"Unexpected pinned source: {code}")
        text = text.replace(api_anchor, api, 1)

    code.write_text(text)


def extend_direct_connections(source):
    """WUP_THIN_PROTOCOL: direct/unicast pad discovery + observed source port.

    'B' beacons may arrive unicast (a ThinClient dialing a specific server
    endpoint) or broadcast (LAN discovery); either way the pad's source
    port is whatever its socket/NAT gave it, not necessarily 50000, so the
    observed port is recorded and kept updated from session packets.
    wupstream_open_port() binds the discovery socket to a configurable UDP
    port (default 50001) and optionally a specific local address;
    wupstream_open() keeps the historical signature for existing callers.
    """
    header = source / "host/wupstream.h"
    code = source / "host/wupstream.c"

    decl_anchor = "wupstream *wupstream_open(const char *ip, int timeout_ms);"
    decls = decl_anchor + """
wupstream *wupstream_open_port(const char *ip, int port, const char *bind_addr,
                               int timeout_ms);   /* port<=0: 50001; bind_addr: numeric IPv4 or NULL/"" for any */"""
    text = header.read_text()
    if decls not in text:
        if text.count(decl_anchor) != 1:
            raise RuntimeError(f"Unexpected pinned header: {header}")
        header.write_text(text.replace(decl_anchor, decls, 1))

    # rename the implementation and give it the new parameters
    sig_old = "wupstream *wupstream_open(const char *ip, int timeout_ms) {"
    sig_new = ("wupstream *wupstream_open(const char *ip, int timeout_ms) {\n"
               "    return wupstream_open_port(ip, 0, NULL, timeout_ms);\n}\n"
               "wupstream *wupstream_open_port(const char *ip, int port,\n"
               "                               const char *bind_addr, int timeout_ms) {")
    text = code.read_text()
    if sig_new not in text:
        if text.count(sig_old) != 1:
            raise RuntimeError(f"Unexpected pinned source: {code}")
        text = text.replace(sig_old, sig_new, 1)

    # discovery socket: configurable port + optional bind address
    port_old = "        me.sin_port = htons(50001);"
    port_new = ("        me.sin_port = htons(port > 0 && port < 65536 ? port : 50001);\n"
                "        if (bind_addr && bind_addr[0]) {\n"
                "            unsigned long a = inet_addr(bind_addr);\n"
                "            me.sin_addr.s_addr = a == INADDR_NONE ? htonl(INADDR_ANY) : a;\n"
                "        }")
    if port_new not in text:
        if text.count(port_old) != 1:
            raise RuntimeError(f"Unexpected pinned source: {code}")
        text = text.replace(port_old, port_new, 1)

    # record the beacon's real source port: direct clients may not use 50000
    beacon_old = ("if (recvfrom(find, (char *)b, sizeof b, 0, (struct sockaddr *)&from, &fl)"
                  " >= 1 && b[0] == 'B') w->pad.sin_addr = from.sin_addr;")
    beacon_new = ("if (recvfrom(find, (char *)b, sizeof b, 0, (struct sockaddr *)&from, &fl)"
                  " >= 1 && b[0] == 'B') { w->pad.sin_addr = from.sin_addr;"
                  " w->pad.sin_port = from.sin_port; }")
    if beacon_new not in text:
        if text.count(beacon_old) != 1:
            raise RuntimeError(f"Unexpected pinned source: {code}")
        text = text.replace(beacon_old, beacon_new, 1)

    # keep the peer port fresh only from validated session packets: an
    # arbitrary datagram from the same IP must not redirect the session.
    # 'B' is accepted too — it is the defined NAT-rebinding path (the pad
    # re-beacons from its current source endpoint and the session resumes).
    pump_old = "        if (from.sin_addr.s_addr != w->pad.sin_addr.s_addr) continue;"
    pump_new = pump_old + """
        int peer_packet = (b[0] == 'B' && n >= 4) ||
                          (b[0] == 'N' && n >= 2 && n >= 4 + 8 * b[1]) ||
                          (b[0] == 'M' && n > 8) ||
                          (b[0] == 'T' && n >= 4) ||
                          (b[0] == 'I' && n == 136);
        if (!peer_packet) continue;
        w->pad.sin_port = from.sin_port;"""
    # migrate the earlier unconditional variant if the cache already has it
    stale = (pump_old + "\n        w->pad.sin_port = from.sin_port;")
    if pump_new not in text:
        if stale in text:
            text = text.replace(stale, pump_new, 1)
        elif text.count(pump_old) == 1:
            text = text.replace(pump_old, pump_new, 1)
        else:
            raise RuntimeError(f"Unexpected pinned source: {code}")

    code.write_text(text)


def copy_atomic(src, target):
    tmp = target.with_name(target.name + ".tmp")
    shutil.copyfile(src, tmp)
    os.replace(tmp, target)


def main(argv):
    if len(argv) != 3:
        raise SystemExit("usage: build_wupstream_dep.py OUT_LIB_A OUT_HEADER_H")
    out_lib, out_header = (Path(v).resolve() for v in argv[1:])
    if out_lib.parent != out_header.parent:
        raise SystemExit("Meson outputs must share the same build directory")
    source = out_lib.parent / "_wupstream_source"
    prepare_source(source)
    extend_i420(source)
    extend_brightness_commands(source)
    extend_control_packets(source)
    extend_direct_connections(source)
    # The upstream build script already pins x264 and applies the DRH patch.
    run(["sh", "host/build.sh"], cwd=source)
    archive = source / "build" / "host" / "libwupstream.a"
    if not archive.is_file() or archive.stat().st_size == 0:
        raise RuntimeError(f"Library not produced: {archive}")
    copy_atomic(archive, out_lib)
    copy_atomic(source / "host" / "wupstream.h", out_header)
    print(f"Built libwupstream locally: {out_lib} ({out_lib.stat().st_size} bytes)",
          flush=True)


if __name__ == "__main__":
    try:
        main(sys.argv)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"ERROR: libwupstream compilation failed: {exc}", file=sys.stderr)
        sys.exit(1)
