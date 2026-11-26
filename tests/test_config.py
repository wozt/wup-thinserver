#!/usr/bin/env python3
"""Config validation tests for wup-thinserver (stdlib unittest).

Runs the real binary with --check against crafted HOME directories so the
defaults file is never touched.
"""
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BINARY = ROOT / "build" / "wup-thinserver"


def run_check(home: Path, config: dict | None = None) -> subprocess.CompletedProcess:
    cfgdir = home / ".config" / "wup-thinserver"
    cfgdir.mkdir(parents=True, exist_ok=True)
    if config is not None:
        (cfgdir / "settings.json").write_text(json.dumps(config))
    env = dict(os.environ, HOME=str(home), DISPLAY=":99")
    return subprocess.run(
        [str(BINARY), "--check"], env=env,
        capture_output=True, text=True, timeout=15,
    )


class ConfigTest(unittest.TestCase):
    def setUp(self):
        self.home = Path(tempfile.mkdtemp())

    def test_defaults_valid(self):
        r = run_check(self.home)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("configuration OK", r.stdout)
        # daemon defaults: stream on connect, kb/mouse input, X -> onboard
        self.assertIn("mode=1", r.stdout)
        self.assertIn("onboard-toggle", r.stdout)

    def test_invalid_source_type(self):
        r = run_check(self.home, {"source_type": 7})
        self.assertEqual(r.returncode, 2)
        self.assertIn("source_type", r.stderr)

    def test_invalid_capture_backend(self):
        r = run_check(self.home, {"capture_backend": 9})
        self.assertEqual(r.returncode, 2)

    def test_invalid_cursor_visibility(self):
        r = run_check(self.home, {"cursor_visible": 2})
        self.assertEqual(r.returncode, 2)
        self.assertIn("cursor_visible", r.stderr)

    def test_invalid_crf(self):
        self.assertEqual(run_check(self.home, {"crf": 99}).returncode, 2)
        self.assertEqual(run_check(self.home, {"crf": -1}).returncode, 2)

    def test_invalid_kbps(self):
        self.assertEqual(run_check(self.home, {"kbps": -5}).returncode, 2)
        self.assertEqual(run_check(self.home, {"kbps": 999999}).returncode, 2)

    def test_invalid_input_mode(self):
        self.assertEqual(
            run_check(self.home, {"input": {"mode": 3}}).returncode, 2)

    def test_invalid_deadzone(self):
        self.assertEqual(
            run_check(self.home, {"input": {"deadzone": 999}}).returncode, 2)

    def test_region_requires_geometry(self):
        self.assertEqual(
            run_check(self.home, {"source_type": 2,
                                  "source": {"w": 0, "h": 0}}).returncode, 2)
        r = run_check(self.home, {"source_type": 2,
                                  "source": {"w": 400, "h": 300}})
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_pad_brightness_range(self):
        for bad in (-2, 0, 7, 100):
            r = run_check(self.home, {"pad_brightness": bad})
            self.assertEqual(r.returncode, 2, (bad, r.stdout, r.stderr))
            self.assertIn("pad_brightness", r.stderr)
        for good in (-1, 1, 6):
            r = run_check(self.home, {"pad_brightness": good})
            self.assertEqual(r.returncode, 0, (good, r.stderr))

    def test_listen_port_range(self):
        for bad in (0, -1, 65536, 99999):
            r = run_check(self.home, {"listen_port": bad})
            self.assertEqual(r.returncode, 2, (bad, r.stdout, r.stderr))
            self.assertIn("listen_port", r.stderr)
        for good in (1, 50001, 65535):
            r = run_check(self.home, {"listen_port": good})
            self.assertEqual(r.returncode, 0, (good, r.stderr))

    def test_listen_address_ipv4(self):
        r = run_check(self.home, {"listen_address": "192.168.1.10"})
        self.assertEqual(r.returncode, 0, r.stderr)
        for bad in ("example.com", "999.1.1.1", "::1", "192.168.1.300"):
            r = run_check(self.home, {"listen_address": bad})
            self.assertEqual(r.returncode, 2, (bad, r.stdout, r.stderr))
            self.assertIn("listen_address", r.stderr)

    def test_commands_and_mouse_buttons_accepted(self):
        r = run_check(self.home, {"input": {
            "commands": [""] * 17 + ["onboard-toggle"],
            "mouse_buttons": [0] * 17 + [1],
        }})
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_malformed_json_uses_defaults(self):
        cfgdir = self.home / ".config" / "wup-thinserver"
        cfgdir.mkdir(parents=True)
        (cfgdir / "settings.json").write_text("{not json")
        r = run_check(self.home)
        self.assertEqual(r.returncode, 0, r.stderr)


if __name__ == "__main__":
    unittest.main()
