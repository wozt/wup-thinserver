#!/usr/bin/env python3
"""End-to-end tests for the same-user local configuration interface."""
import json
import os
import socket
import stat
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BINARY = ROOT / "build" / "wup-thinserver"


class ControlTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        cls.home = cls.root / "home"
        cls.runtime = cls.root / "runtime"
        cls.home.mkdir(mode=0o700)
        cls.runtime.mkdir(mode=0o700)
        config_dir = cls.home / ".config" / "wup-thinserver"
        config_dir.mkdir(parents=True)
        cls.config_path = config_dir / "settings.json"
        cls.config_path.write_text(json.dumps({"autostart_stream": 0}))

        cls.xvfb = subprocess.Popen(
            ["Xvfb", "-displayfd", "1", "-screen", "0", "854x480x24", "-nolisten", "tcp"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        display_number = cls.xvfb.stdout.readline().strip()
        if not display_number:
            raise RuntimeError("Xvfb did not allocate a display")
        env = dict(os.environ, HOME=str(cls.home), XDG_RUNTIME_DIR=str(cls.runtime),
                   DISPLAY=f":{display_number}")
        cls.daemon = subprocess.Popen(
            [str(BINARY)], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True,
        )
        cls.sock = cls.runtime / "wup-thinserver" / "control.sock"
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and not cls.sock.exists():
            if cls.daemon.poll() is not None:
                raise RuntimeError(cls.daemon.stderr.read())
            time.sleep(0.02)
        if not cls.sock.exists():
            raise RuntimeError("control socket was not created")

    @classmethod
    def tearDownClass(cls):
        cls.daemon.terminate()
        try:
            cls.daemon.wait(timeout=5)
        except subprocess.TimeoutExpired:
            cls.daemon.kill()
        cls.xvfb.terminate()
        try:
            cls.xvfb.wait(timeout=3)
        except subprocess.TimeoutExpired:
            cls.xvfb.kill()
        cls.temp.cleanup()

    @classmethod
    def request(cls, payload):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(3)
            connection.connect(str(cls.sock))
            connection.sendall(json.dumps(payload).encode() + b"\n")
            connection.shutdown(socket.SHUT_WR)
            data = b""
            while not data.endswith(b"\n"):
                data += connection.recv(65536)
        return json.loads(data)

    def test_socket_permissions_and_get(self):
        self.assertEqual(stat.S_IMODE(self.sock.stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(self.sock.parent.stat().st_mode), 0o700)
        response = self.request({"op": "get"})
        self.assertTrue(response["ok"])
        self.assertEqual(len(response["config"]["input"]["keys"]), 18)
        self.assertEqual(response["config"]["input"]["mode"], 1)
        self.assertIn(response["state"]["input_mode"], ("desktop", "gamepad"))
        self.assertIn("gamepad_available", response["state"])
        self.assertTrue(response["state"]["input_status"])
        self.assertFalse(response["config"]["camera_announce"])
        self.assertFalse(response["config"]["camera_preview"])
        self.assertEqual(response["config"]["camera_device"], "/dev/video42")
        self.assertIn("camera", response["state"])
        self.assertEqual(response["state"]["camera"]["status"], "Camera inactive")

    def test_camera_controls_are_independent_and_validated(self):
        before = self.config_path.read_bytes()
        for payload in (
            {"op": "set_camera", "camera_announce": "yes"},
            {"op": "set_camera", "camera_preview": 7},
            {"op": "set_camera", "camera_device": "/dev/video42/other"},
            {"op": "set_camera", "camera_device": "/dev/video"},
        ):
            response = self.request(payload)
            self.assertFalse(response["ok"], (payload, response))
            self.assertEqual(self.config_path.read_bytes(), before)

        response = self.request({
            "op": "set_camera", "camera_announce": True,
            "camera_preview": False, "camera_device": "/dev/video43",
        })
        self.assertTrue(response["ok"], response)
        saved = json.loads(self.config_path.read_text())
        self.assertEqual(saved["camera_announce"], 1)
        self.assertEqual(saved["camera_preview"], 0)
        self.assertEqual(saved["camera_device"], "/dev/video43")

        response = self.request({
            "op": "set_camera", "camera_announce": False,
            "camera_preview": True,
        })
        self.assertTrue(response["ok"], response)
        saved = json.loads(self.config_path.read_text())
        self.assertEqual(saved["camera_announce"], 0)
        self.assertEqual(saved["camera_preview"], 1)
        self.assertEqual(saved["camera_device"], "/dev/video43")

        restored = self.request({
            "op": "set_camera", "camera_announce": False,
            "camera_preview": False, "camera_device": "/dev/video42",
        })
        self.assertTrue(restored["ok"], restored)

    def test_gamepad_mode_preflight_and_persistence(self):
        response = self.request({"op": "get"})
        current = response["config"]["input"]
        original = current["mode"]
        current["mode"] = 0
        switched = self.request({"op": "apply_input", "input": current})
        if response["state"]["gamepad_available"]:
            self.assertTrue(switched["ok"], switched)
            self.assertEqual(json.loads(self.config_path.read_text())["input"]["mode"], 0)
        else:
            self.assertFalse(switched["ok"])
            self.assertIn("Gamepad mode unavailable", switched["error"])
        current["mode"] = original
        restored = self.request({"op": "apply_input", "input": current})
        self.assertTrue(restored["ok"], restored)

    def test_invalid_binding_is_rejected_without_file_change(self):
        current = self.request({"op": "get"})["config"]["input"]
        before = self.config_path.read_bytes()
        current["keys"][0] = "Ctrl+Definitely_Not_A_Key"
        response = self.request({"op": "apply_input", "input": current})
        self.assertFalse(response["ok"])
        self.assertIn("unknown key", response["error"])
        self.assertEqual(self.config_path.read_bytes(), before)

    def test_apply_is_live_and_persisted_atomically(self):
        current = self.request({"op": "get"})["config"]["input"]
        current["keys"][0] = "F6"
        current["deadzone"] = 140
        response = self.request({"op": "apply_input", "input": current})
        self.assertTrue(response["ok"], response)
        loaded = json.loads(self.config_path.read_text())
        self.assertEqual(loaded["input"]["keys"][0], "F6")
        self.assertEqual(loaded["input"]["deadzone"], 140)
        self.assertEqual(stat.S_IMODE(self.config_path.stat().st_mode), 0o600)

    def test_arbitrary_command_is_rejected(self):
        current = self.request({"op": "get"})["config"]["input"]
        current["commands"][0] = "touch /tmp/not-allowed"
        response = self.request({"op": "apply_input", "input": current})
        self.assertFalse(response["ok"])
        self.assertIn("built-in commands", response["error"])

    def test_pad_brightness_op(self):
        config = self.request({"op": "get"})["config"]
        self.assertIn("pad_brightness", config)
        before = self.config_path.read_bytes()
        for bad in (-2, 0, 7):
            response = self.request({"op": "set_pad_brightness", "level": bad})
            self.assertFalse(response["ok"], (bad, response))
        self.assertEqual(self.config_path.read_bytes(), before)
        for good in (1, 4, 6, -1):
            response = self.request({"op": "set_pad_brightness", "level": good})
            self.assertTrue(response["ok"], (good, response))
            saved = json.loads(self.config_path.read_text())
            self.assertEqual(saved["pad_brightness"], good)
            self.assertEqual(
                self.request({"op": "get"})["config"]["pad_brightness"], good)

    def test_restore_defaults(self):
        response = self.request({"op": "restore_defaults"})
        self.assertTrue(response["ok"])
        self.assertEqual(response["config"]["input"]["deadzone"], 120)
        self.assertEqual(response["config"]["input"]["commands"][2], "onboard-toggle")
        self.assertFalse(response["config"]["camera_announce"])
        self.assertFalse(response["config"]["camera_preview"])
        self.assertEqual(response["config"]["camera_device"], "/dev/video42")


if __name__ == "__main__":
    unittest.main()
