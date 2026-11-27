#!/usr/bin/env python3
"""Publish the next incremental four-part release and prune obsolete ones.

Versioning: release/version holds the base series (e.g. "1.0.0"); each
successful main build allocates "v<base>.<N>" with N incrementing, using
GitHub's atomic git refs API so concurrent runners cannot collide. Reruns
reuse the tag already pointing at the same commit and repair missing assets.

Retention: the earliest release ever published is kept forever, plus only
the NEWEST build of every major.minor.patch series that has releases. Older
incremental builds have both their GitHub release and their git tag
deleted. Example with base bumps to 1.0.1: v1.0.0.0, latest 1.0.0.x and
latest 1.0.1.x survive; everything else is removed.

Requires GH_TOKEN, GITHUB_REPOSITORY and GITHUB_SHA (GitHub Actions).
"""
from __future__ import annotations

import os
from pathlib import Path
import re
import subprocess
import sys
import time

MAX_TAG_ATTEMPTS = 15
VERSION_FILE = "release/version"
TAG_RE = re.compile(r"^v(\d+)\.(\d+)\.(\d+)\.(\d+)$")


def sh(*args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(args, text=True, capture_output=True,
                            check=False, env=os.environ.copy())
    if check and result.returncode:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(args)}\n"
            f"{result.stderr[-3000:]}"
        )
    return result


def base_version(root: Path) -> str:
    raw = (root / VERSION_FILE).read_text().strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+", raw):
        raise RuntimeError(f"{VERSION_FILE} must contain X.Y.Z, got {raw!r}")
    return raw


def tag_series(tag: str) -> tuple[int, int, int, int] | None:
    m = TAG_RE.fullmatch(tag)
    return tuple(int(g) for g in m.groups()) if m else None


def remote_tags(repo: str) -> dict[str, str]:
    """Map tag name -> commit sha for all vX.Y.Z.N tags."""
    out = sh("gh", "api", f"repos/{repo}/git/matching-refs/tags/v",
             "--paginate", "--jq", ".[].ref").stdout
    tags = {}
    for ref in out.split():
        if not ref.startswith("refs/tags/"):
            continue
        name = ref.removeprefix("refs/tags/")
        if tag_series(name):
            sha = sh("gh", "api", f"repos/{repo}/git/ref/tags/{name}",
                     "--jq", ".object.sha").stdout.strip()
            tags[name] = sha
    return tags


def choose_tag(tags: dict[str, str], base: str, sha: str) -> tuple[str, bool]:
    """Reuse this commit's existing tag, else allocate the next index."""
    prefix = f"v{base}."
    for name, tag_sha in tags.items():
        if tag_sha == sha and name.startswith(prefix):
            return name, True
    indexes = [int(n.removeprefix(prefix)) for n in tags
               if n.startswith(prefix) and n.removeprefix(prefix).isdigit()]
    return f"{prefix}{max(indexes, default=-1) + 1}", False


def reserve_tag(repo: str, base: str, sha: str) -> str:
    for attempt in range(MAX_TAG_ATTEMPTS):
        tags = remote_tags(repo)
        tag, reused = choose_tag(tags, base, sha)
        if reused:
            print(f"Reusing {tag} for this commit ({sha[:12]}).", flush=True)
            return tag
        result = sh("gh", "api", "-X", "POST", f"repos/{repo}/git/refs",
                    "-f", f"ref=refs/tags/{tag}", "-f", f"sha={sha}",
                    check=False)
        if result.returncode == 0:
            print(f"Reserved new version {tag} ({sha[:12]}).", flush=True)
            return tag
        updated = remote_tags(repo)
        if tag in updated:
            if updated[tag] == sha:
                return tag
            print(f"{tag} claimed by another build; retrying.", flush=True)
            time.sleep(min(1 + attempt, 5))
            continue
        raise RuntimeError(f"Cannot create {tag}: {result.stderr[-2000:]}")
    raise RuntimeError("Too many concurrent tag collisions")


def release_names(repo: str) -> set[str]:
    proc = sh("gh", "api", f"repos/{repo}/releases?per_page=100",
              "--paginate", "--jq", ".[].tag_name", check=False)
    return set(proc.stdout.split())


def prune_releases(repo: str) -> None:
    """Keep the earliest-ever release plus the newest build per series."""
    proc = sh("gh", "api", f"repos/{repo}/releases?per_page=100",
              "--paginate",
              "--jq", ".[] | [.tag_name, .id] | @tsv", check=False)
    entries = []
    for line in proc.stdout.splitlines():
        parts = line.split()
        if len(parts) == 2 and tag_series(parts[0]):
            entries.append((parts[0], int(parts[1])))
    if not entries:
        return

    by_series: dict[tuple[int, int, int], list[str]] = {}
    for tag, _ in entries:
        major, minor, patch, build = tag_series(tag)
        by_series.setdefault((major, minor, patch), []).append(tag)

    keep = {min(t for t, _ in entries)}          # earliest release ever
    for series_tags in by_series.values():
        keep.add(max(series_tags, key=lambda t: tag_series(t)[3]))

    for tag, release_id in entries:
        if tag in keep:
            continue
        print(f"Pruning obsolete release {tag}", flush=True)
        sh("gh", "api", "-X", "DELETE",
           f"repos/{repo}/releases/{release_id}", check=False)
        # Deleting the release leaves the tag; remove it too (annotated or
        # lightweight refs both delete via the matching-refs endpoint).
        sh("gh", "api", "-X", "DELETE",
           f"repos/{repo}/git/refs/tags/{tag}", check=False)


def main() -> None:
    if len(sys.argv) >= 2 and sys.argv[1] == "--next-tag":
        # Print the tag the next release would allocate, without creating it.
        # Used by the workflow to pin docker images and render the compose
        # file before publish_release.py reserves the ref.
        repo = os.environ.get("GITHUB_REPOSITORY", "")
        sha = os.environ.get("GITHUB_SHA", "")
        if not re.fullmatch(r"[\w.-]+/[\w.-]+", repo):
            raise RuntimeError("GITHUB_REPOSITORY is absent or invalid")
        if not re.fullmatch(r"[0-9a-f]{40}", sha):
            raise RuntimeError("GITHUB_SHA is absent or invalid")
        root = Path(__file__).resolve().parent.parent
        tag, _ = choose_tag(remote_tags(repo), base_version(root), sha)
        print(tag)
        return
    if len(sys.argv) < 2:
        raise SystemExit("usage: publish_release.py [--next-tag] <assets-dir> [more-assets...]")
    repo = os.environ.get("GITHUB_REPOSITORY", "")
    sha = os.environ.get("GITHUB_SHA", "")
    if not re.fullmatch(r"[\w.-]+/[\w.-]+", repo):
        raise RuntimeError("GITHUB_REPOSITORY is absent or invalid")
    if not re.fullmatch(r"[0-9a-f]{40}", sha):
        raise RuntimeError("GITHUB_SHA is absent or invalid")
    if not os.environ.get("GH_TOKEN"):
        raise RuntimeError("GH_TOKEN is required to create tags and releases")

    root = Path(__file__).resolve().parent.parent
    base = base_version(root)

    assets = []
    for arg in sys.argv[1:]:
        path = Path(arg)
        if path.is_dir():
            assets.extend(sorted(p for p in path.iterdir() if p.is_file()))
        elif path.is_file():
            assets.append(path)
    if not assets:
        raise RuntimeError("no release assets found")

    forced = os.environ.get("RELEASE_TAG", "")
    if forced:
        if tag_series(forced) is None:
            raise RuntimeError(f"RELEASE_TAG is not vX.Y.Z.N: {forced}")
        tag = forced
        if tag not in remote_tags(repo):
            sh("gh", "api", "-X", "POST", f"repos/{repo}/git/refs",
               "-f", f"ref=refs/tags/{tag}", "-f", f"sha={sha}")
        print(f"Using forced tag {tag}.", flush=True)
    else:
        tag = reserve_tag(repo, base, sha)
    names = release_names(repo)

    if tag in names:
        existing = sh("gh", "release", "view", tag, "--repo", repo,
                      "--json", "assets", "--jq",
                      ".assets[].name", check=False)
        have = set(existing.stdout.split())
        missing = [a for a in assets if a.name not in have]
        if missing:
            sh("gh", "release", "upload", tag,
               *[str(a) for a in missing], "--clobber", "--repo", repo)
            print(f"Repaired assets of {tag}.")
        else:
            print(f"Release {tag} already complete; nothing to upload.")
    else:
        sh("gh", "release", "create", tag,
           *[str(a) for a in assets],
           "--verify-tag", "--repo", repo,
           "--title", tag.removeprefix("v"),
           "--generate-notes")
        print(f"Published release {tag} with {len(assets)} assets.")

    prune_releases(repo)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"Release failed: {exc}", file=sys.stderr)
        sys.exit(1)
