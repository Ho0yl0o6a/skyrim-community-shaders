"""Plane-side regression for thin-surface visibility ray origins.

This checks the shader expression and the plane-intersection invariant, not GPU
intersection precision or the visual result in Skyrim.
"""

import math
from pathlib import Path
import unittest


RAY_SHADER = Path(__file__).resolve().parents[2] / (
    ".research/dxvk-remix/src/dxvk/shaders/rtx/concept/ray/ray.slangh"
)


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def plane_hit(normal, direction, side_normal):
    """Offset by one unit along the geometric normal; intersect its plane."""
    sign = -1.0 if dot(side_normal, direction) <= 0.0 else 1.0
    denominator = dot(normal, direction)
    return math.inf if denominator == 0.0 else -sign / denominator


class ThinRayOffsetTests(unittest.TestCase):
    def test_shader_uses_offset_geometry_for_side(self):
        shader = RAY_SHADER.read_text(encoding="utf-8")
        body = shader.split("Ray rayCreatePositionSubsurface(", 1)[1].split(
            "// Evaluates a position", 1
        )[0]
        self.assertIn("dot(minimalSurfaceInteraction.geometryNormal, direction)", body)
        self.assertNotIn("dot(shadingNormal, direction)", body)

    def test_shading_normal_mismatch_reproduces_self_hit(self):
        normal = (0.0, 0.0, 1.0)
        shading = (math.sqrt(0.75), 0.0, 0.5)
        for direction in ((0.8, 0.0, -0.6), (-0.8, 0.0, 0.6)):
            with self.subTest(direction=direction):
                self.assertGreater(plane_hit(normal, direction, shading), 0.0)
                self.assertLess(plane_hit(normal, direction, normal), 0.0)

    def test_both_sides_and_rotated_planes(self):
        for normal in ((0, 0, 1), (0, 1, 0), (1, 0, 0), (0.6, 0.8, 0)):
            for x in range(-5, 6):
                for y in range(-5, 6):
                    for z in range(-5, 6):
                        direction = (x / 5, y / 5, z / 5)
                        if abs(dot(normal, direction)) > 1e-6:
                            self.assertLess(plane_hit(normal, direction, normal), 0.0)


if __name__ == "__main__":
    unittest.main()
