import unittest

import numpy as np

from ProbeBufferRegistration import fit_depth_shift, sample_points


class RegistrationTests(unittest.TestCase):
    def test_pixel_centres_and_bilinear_plane(self):
        y, x = np.indices((8, 9))
        values = 100 + x * 2 + y * 3
        np.testing.assert_equal(sample_points(values, x, y), values)
        self.assertAlmostEqual(float(sample_points(values, np.array(2.25), np.array(3.5), True)), 115)

    def test_edges_rejected_not_wrapped(self):
        values = np.ones((5, 5))
        for bilinear in (False, True):
            with self.assertRaises(ValueError):
                sample_points(values, np.array(-1), np.array(2), bilinear)
            with self.assertRaises(ValueError):
                sample_points(values, np.array(5), np.array(2), bilinear)

    def test_half_pixel_tie_is_explicit_not_bilinear_normals(self):
        values = np.arange(25).reshape(5, 5)
        self.assertEqual(sample_points(values, np.array(2.), np.array(2.)), 12)
        self.assertEqual(sample_points(values, np.array(2.), np.array(1.5)), 12)
        self.assertEqual(sample_points(values, np.array(2.), np.array(1.499)), 7)

    def test_known_global_shift_recovered_without_normals(self):
        yy, xx = np.indices((25, 25))
        values = 100 + xx * xx + yy * yy * .7 + xx * yy * .15
        y, x = np.indices((10, 10), dtype=float)
        x, y = x + 5.2, y + 5.1
        target = sample_points(values, x + .5, y - .25, True)
        ranked = fit_depth_shift(values, target, x, y, np.arange(-1, 1.001, .25))
        self.assertEqual(ranked[0]["nativePixelShift"], [.5, -.25])
        self.assertEqual(ranked[0]["depthRelativeMAE"], 0)

    def test_invalid_or_empty_calibration_rejected(self):
        values = np.ones((5, 5))
        for target in (np.array([]), np.array([np.nan]), np.array([0])):
            with self.assertRaises(ValueError):
                fit_depth_shift(values, target, np.ones_like(target), np.ones_like(target), [0])
        values[1, 1] = np.nan
        with self.assertRaises(ValueError):
            fit_depth_shift(values, np.ones(1), np.ones(1), np.ones(1), [0])


if __name__ == "__main__":
    unittest.main()
