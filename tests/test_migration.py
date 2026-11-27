#!/usr/bin/env python3
"""Persisted camera-setting migration tests."""
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MIGRATE = ROOT / "scripts" / "migrate-settings.py"


class MigrationTest(unittest.TestCase):
    def run_migration(self, config):
        temporary = tempfile.TemporaryDirectory()
        path = Path(temporary.name) / "settings.json"
        path.write_text(json.dumps(config))
        env = dict(os.environ, WUP_THIN_CONFIG=str(path))
        result = subprocess.run(
            [sys.executable, str(MIGRATE)], env=env,
            capture_output=True, text=True, timeout=10
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return temporary, path, result

    def test_missing_camera_settings_are_added_disabled(self):
        temporary, path, _ = self.run_migration({"listen_port": 50002})
        try:
            migrated = json.loads(path.read_text())
            self.assertEqual(migrated["camera_announce"], 0)
            self.assertEqual(migrated["camera_preview"], 0)
            self.assertEqual(migrated["camera_device"], "/dev/video42")
            self.assertTrue(Path(str(path) + ".pre-desktop-migration.bak").is_file())

            before = path.read_bytes()
            env = dict(os.environ, WUP_THIN_CONFIG=str(path))
            second = subprocess.run(
                [sys.executable, str(MIGRATE)], env=env,
                capture_output=True, text=True, timeout=10
            )
            self.assertEqual(second.returncode, 0, second.stderr)
            self.assertEqual(path.read_bytes(), before)
            self.assertEqual(second.stdout, "")
        finally:
            temporary.cleanup()

    def test_existing_camera_settings_are_preserved(self):
        configured = {
            "camera_announce": 1,
            "camera_preview": 1,
            "camera_device": "/dev/video77",
        }
        temporary, path, result = self.run_migration(configured)
        try:
            self.assertEqual(json.loads(path.read_text()), configured)
            self.assertEqual(result.stdout, "")
            self.assertFalse(Path(str(path) + ".pre-desktop-migration.bak").exists())
        finally:
            temporary.cleanup()


if __name__ == "__main__":
    unittest.main()
