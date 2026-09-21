"""CPU precision/contract tests, not a GPU shader execution test."""

from pathlib import Path
import re
import unittest

import numpy as np


class PrimaryRayPrecisionTests(unittest.TestCase):
    def test_half_direction_changes_hit_position(self):
        direction = np.array([0.863173, 0.493219, 0.10523], dtype=np.float32)
        direction /= np.linalg.norm(direction)
        distance = np.float32(65)
        rounded = direction.astype(np.float16).astype(np.float32)
        error = np.linalg.norm(distance * rounded - distance * direction)
        self.assertGreater(error, .005)
        np.testing.assert_array_equal(distance * direction.copy(), distance * direction)

    def test_half_direction_moves_continuation_across_thin_layer(self):
        # A nearby layer can be closer than the hit displacement caused by f16.
        direction = np.array([0.863173, 0.493219, 0.10523], dtype=np.float32)
        direction /= np.linalg.norm(direction)
        rounded = direction.astype(np.float16).astype(np.float32)
        exact = direction * np.float32(65)
        drift = rounded * np.float32(65) - exact
        plane_normal = drift / np.linalg.norm(drift)
        plane_offset = np.dot(plane_normal, exact) + .002
        self.assertLess(np.dot(plane_normal, exact), plane_offset)
        self.assertGreater(np.dot(plane_normal, exact + drift), plane_offset)

    def test_both_gbuffer_payloads_preserve_float_direction(self):
        source = (Path(__file__).resolve().parents[2] /
                  '.research/dxvk-remix/src/dxvk/shaders/rtx/algorithm/geometry_resolver_state.slangh').read_text()
        self.assertEqual(len(re.findall(r'^  vec3 _direction;', source, re.M)), 2)
        self.assertNotRegex(source, r'f16vec3 _direction|_direction = f16vec3\(newValue\)')
        self.assertEqual(source.count('set { _direction = newValue; }'), 2)


if __name__ == '__main__':
    unittest.main()
