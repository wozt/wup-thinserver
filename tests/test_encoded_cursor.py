#!/usr/bin/env python3
"""Verify that cursor composition changes the actual WUP video payload."""

import hashlib
import json
import os
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BINARY = ROOT / "build" / "wup-thinserver"


class FakePad(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.payload = None
        self.error = None

    def run(self):
        try:
            self.payload = self._receive_frame()
        except Exception as exc:  # reported by the main test thread
            self.error = exc

    @staticmethod
    def _input_packet(sequence):
        packet = bytearray(136)
        packet[0] = ord("I")
        packet[1] = 1  # request the initial keyframe
        packet[4] = 3  # backlight level
        packet[8:10] = struct.pack("<H", sequence & 0xffff)
        for axis in range(4):
            packet[8 + 6 + axis * 2:8 + 8 + axis * 2] = b"\x00\x08"
        return packet

    def _receive_frame(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as pad:
            pad.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            pad.bind(("127.0.0.1", 50000))
            pad.settimeout(0.01)
            peer = None
            sequence = 0
            frames = {}
            deadline = time.monotonic() + 12
            next_input = 0.0
            while time.monotonic() < deadline:
                now = time.monotonic()
                if peer and now >= next_input:
                    pad.sendto(self._input_packet(sequence), peer)
                    sequence += 1
                    next_input = now + 0.002
                try:
                    data, address = pad.recvfrom(1600)
                except socket.timeout:
                    continue
                if data[:1] == b"H":
                    peer = address
                    continue
                if data[:1] != b"V" or len(data) < 16:
                    continue
                frame, offset, total = struct.unpack_from("<III", data, 4)
                entry = frames.setdefault(frame, {"total": total, "parts": {}})
                entry["parts"][offset] = data[16:]
                received = sum(len(part) for part in entry["parts"].values())
                if received >= total:
                    payload = bytearray(total)
                    for part_offset, part in entry["parts"].items():
                        payload[part_offset:part_offset + len(part)] = part
                    return bytes(payload)
            raise TimeoutError("no complete encoded video frame received")


class EncodedCursorTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        cls.xvfb = subprocess.Popen(
            ["Xvfb", "-displayfd", "1", "-screen", "0", "1280x720x24",
             "-nolisten", "tcp"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        number = cls.xvfb.stdout.readline().strip()
        if not number:
            raise RuntimeError("Xvfb did not allocate a display")
        cls.display = f":{number}"
        env = dict(os.environ, DISPLAY=cls.display)
        subprocess.run(["xsetroot", "-solid", "#204060", "-cursor_name", "crosshair"],
                       env=env, check=True)
        subprocess.run(["xdotool", "mousemove", "640", "360"], env=env, check=True)

    @classmethod
    def tearDownClass(cls):
        cls.xvfb.terminate()
        try:
            cls.xvfb.wait(timeout=3)
        except subprocess.TimeoutExpired:
            cls.xvfb.kill()
        cls.temp.cleanup()

    def encode(self, cursor_visible):
        home = self.root / f"home-{cursor_visible}"
        runtime = self.root / f"runtime-{cursor_visible}"
        config_dir = home / ".config" / "wup-thinserver"
        config_dir.mkdir(parents=True)
        runtime.mkdir(mode=0o700)
        config = {
            "pad_ip": "127.0.0.1",
            "source_type": 0,
            "aspect": 0,
            "capture_backend": 0,
            "cursor_visible": cursor_visible,
            "audio_pc_to_pad": 0,
            "audio_pad_to_pc": 0,
            "autostart_stream": 1,
            "auto_reconnect": 0,
        }
        (config_dir / "settings.json").write_text(json.dumps(config), encoding="utf-8")
        env = dict(os.environ, HOME=str(home), XDG_RUNTIME_DIR=str(runtime),
                   DISPLAY=self.display)
        pad = FakePad()
        pad.start()
        daemon = subprocess.Popen(
            [str(BINARY)], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True,
        )
        pad.join(timeout=15)
        daemon.terminate()
        try:
            daemon.wait(timeout=5)
        except subprocess.TimeoutExpired:
            daemon.kill()
            daemon.wait(timeout=3)
        if pad.is_alive():
            self.fail("fake GamePad thread did not finish")
        if pad.error:
            self.fail(f"{pad.error}; daemon stderr: {daemon.stderr.read()}")
        self.assertTrue(pad.payload)
        return pad.payload

    def test_cursor_reaches_encoded_video(self):
        without_cursor = self.encode(0)
        with_cursor = self.encode(1)
        print("encoded cursor off:", len(without_cursor),
              hashlib.sha256(without_cursor).hexdigest())
        print("encoded cursor on: ", len(with_cursor),
              hashlib.sha256(with_cursor).hexdigest())
        self.assertNotEqual(hashlib.sha256(without_cursor).digest(),
                            hashlib.sha256(with_cursor).digest())


if __name__ == "__main__":
    unittest.main()
