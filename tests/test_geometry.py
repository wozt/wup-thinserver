#!/usr/bin/env python3
"""Coordinate/geometry contract tests.

The C code under test (capture.c aspect_geom, input.c touch mapping) is
static; these tests pin the documented math so regressions are caught when
the formulas change. FW/FH are the WUP Stream frame size.
"""
import unittest

FW, FH = 854, 480


def aspect_geom(sw, sh, aspect):
    """Mirror of capture.c aspect_geom(): returns (tw, th, ox, oy)."""
    STRETCH, LETTERBOX, CROP = 0, 1, 2
    ox = oy = 0
    if aspect == STRETCH:
        return FW, FH, 0, 0
    sx, sy = FW / sw, FH / sh
    if aspect == LETTERBOX:
        k = min(sx, sy)
        tw, th = int(sw * k), int(sh * k)
        tw, th = min(tw, FW), min(th, FH)
        return tw, th, (FW - tw) // 2, (FH - th) // 2
    k = max(sx, sy)
    tw, th = int(sw * k + 0.5), int(sh * k + 0.5)
    return max(tw, FW), max(th, FH), 0, 0


def touch_map(tx, ty, mx, my, mw, mh):
    """Mirror of input.c TOUCH_PX/TOUCH_PY."""
    return (mx + int(tx * mw / 853.0), my + int(ty * mh / 479.0))


class GeometryTest(unittest.TestCase):
    def test_letterbox_720p(self):
        # 1280x720 -> 16:9 into 854x480 (16:9-ish): fills width, small bars
        tw, th, ox, oy = aspect_geom(1280, 720, 1)
        # int() truncation: 1280 * (480/720) = 853.33 -> 853
        self.assertEqual((tw, th), (853, 480))
        self.assertEqual((ox, oy), (0, 0))

    def test_letterbox_1080p(self):
        tw, th, ox, oy = aspect_geom(1920, 1080, 1)
        self.assertLessEqual(tw, FW)
        self.assertLessEqual(th, FH)
        self.assertGreaterEqual(ox, 0)

    def test_letterbox_pillarbox_4x3(self):
        # 4:3 source in 16:9 frame -> pillarbox bars left/right
        tw, th, ox, oy = aspect_geom(1024, 768, 1)
        self.assertEqual(th, FH)
        self.assertGreater(ox, 0)
        self.assertEqual(oy, 0)

    def test_stretch(self):
        self.assertEqual(aspect_geom(1920, 1080, 0), (FW, FH, 0, 0))

    def test_crop_covers_frame(self):
        tw, th, _, _ = aspect_geom(1280, 720, 2)
        self.assertGreaterEqual(tw, FW)
        self.assertGreaterEqual(th, FH)

    def test_touch_corners(self):
        # full-screen map: pad corners -> desktop corners
        self.assertEqual(touch_map(0, 0, 0, 0, 1280, 720), (0, 0))
        x, y = touch_map(853, 479, 0, 0, 1280, 720)
        self.assertEqual((x, y), (1280, 720))

    def test_touch_region_offset(self):
        # a 640x360 region at (100,50): touch center -> region center
        x, y = touch_map(426, 240, 100, 50, 640, 360)
        self.assertEqual(x, 100 + int(426 * 640 / 853.0))
        self.assertEqual(y, 50 + int(240 * 360 / 479.0))

    def test_touch_clamped_by_construction(self):
        # formula never exceeds map for max pad coords
        x, y = touch_map(853, 479, 10, 20, 500, 300)
        self.assertLessEqual(x, 10 + 500)
        self.assertLessEqual(y, 20 + 300)


if __name__ == "__main__":
    unittest.main()
