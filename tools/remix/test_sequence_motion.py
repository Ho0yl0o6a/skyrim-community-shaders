import unittest
import numpy as np
from MeasureSequenceMotion import shift


class SequenceMotionTests(unittest.TestCase):
    def test_translation_sign(self):
        source = np.random.default_rng(7).normal(size=(128, 128))
        for dx, dy in [(0, 0), (5, -7), (-3, 9)]:
            with self.subTest(dx=dx, dy=dy):
                target = np.roll(source, (dy, dx), axis=(0, 1))
                self.assertEqual(shift(source, target)[:2], [dx, dy])

    def test_shape(self):
        with self.assertRaises(ValueError):
            shift(np.ones((16, 16)), np.ones((15, 15)))
