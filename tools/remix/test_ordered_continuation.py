"""Analytic float32/source contracts; not GPU shader execution tests."""

from pathlib import Path
import unittest

import numpy as np


class OrderedRay:
    def __init__(self, origin, direction, maximum=100):
        self.origin = np.array(origin, dtype=np.float32)
        self.direction = np.array(direction, dtype=np.float32)
        self.maximum = np.float32(maximum)
        self.previous = np.float32(0)
        self.minimum = np.float32(0)
        self.shading_origin = self.origin.copy()

    def hit(self, absolute):
        absolute = np.float32(absolute)
        segment = max(absolute - self.previous, np.float32(0))
        self.previous = absolute
        return segment

    def advance(self, portal=None):
        if portal is None:
            self.minimum = np.nextafter(self.previous, np.float32(np.inf))
            self.shading_origin = self.origin + self.previous * self.direction
        else:
            self.maximum = max(self.maximum - self.previous, np.float32(0))
            self.origin = np.array(portal[0], dtype=np.float32)
            self.direction = np.array(portal[1], dtype=np.float32)
            self.shading_origin = self.origin.copy()
            self.minimum = self.previous = np.float32(0)
        return self.minimum <= self.maximum


class OrderedContinuationTests(unittest.TestCase):
    def test_close_layer_not_skipped_at_large_world_position(self):
        ray = OrderedRay([13735, -48155, -158], [0, 1, 0])
        origin = ray.origin.copy()
        ray.hit(65)
        self.assertTrue(ray.advance())
        self.assertLess(ray.minimum, np.float32(65.01))
        np.testing.assert_array_equal(ray.origin, origin)
        self.assertAlmostEqual(float(ray.hit(65.01)), .01, places=4)
        # Existing world-coordinate normal bias exceeds this separation.
        self.assertGreater(48155 * 4 * 2**-23, .01)

    def test_many_intervals_do_not_double_count_cone_or_attenuation(self):
        ray = OrderedRay([13735, -48155, -158], [0, 1, 0])
        segments = []
        for distance in [65, 65.01, 65.012, 65.1, 70]:
            segments.append(ray.hit(distance))
            ray.advance()
        self.assertEqual(float(np.sum(segments)), 70)
        self.assertAlmostEqual(.2 + .01 * sum(segments), .9, places=6)
        self.assertAlmostEqual(float(np.prod(np.exp(-.1 * np.array(segments)))),
                               float(np.exp(-7)), places=7)

    def test_portal_consumes_absolute_distance_once_then_resets(self):
        ray = OrderedRay([13735, -48155, -158], [0, 1, 0])
        ray.hit(60)
        ray.advance()
        self.assertEqual(ray.hit(70), 10)
        self.assertTrue(ray.advance(portal=([10, 20, 30], [1, 0, 0])))
        self.assertEqual(ray.maximum, 30)
        self.assertEqual(ray.minimum, 0)
        self.assertEqual(ray.hit(4), 4)
        ray.advance()
        np.testing.assert_array_equal(ray.origin, [10, 20, 30])
        np.testing.assert_array_equal(ray.shading_origin, [14, 20, 30])

    def test_exhausted_ray_reports_miss_instead_of_invalid_range(self):
        ray = OrderedRay([0, 0, 0], [0, 0, 1], maximum=2)
        ray.hit(2)
        self.assertFalse(ray.advance())

    def test_both_traversal_paths_initialize_and_advance(self):
        root = Path(__file__).resolve().parents[2] / '.research/dxvk-remix/src/dxvk/shaders/rtx/algorithm'
        source = (root / 'resolve_expanded.slangh').read_text()
        self.assertEqual(source.count('payload.resolveRayT = 0.0f;'), 2)
        self.assertEqual(source.count('payload.continueOriginalRay = false;'), 2)
        self.assertEqual(source.count('if (!advanceResolveRay(currentRay, payload))'), 2)
        self.assertEqual(source.count('hitFunction(extraArgs, exhaustedHit, payload);'), 2)
        for file, state in [('geometry_resolver.slangh', 'geometryResolverState'),
                            ('geometry_resolver.slangh', 'geometryPSRResolverState'),
                            ('integrator_indirect.slangh', 'pathState')]:
            text = (root / file).read_text()
            start = text.index(f'beginResolveSegment(rayHitInfo, {state});')
            self.assertLess(start, text.index('Ray unorderedResolveRay;', start))


if __name__ == '__main__':
    unittest.main()
