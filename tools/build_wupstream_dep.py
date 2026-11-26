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
