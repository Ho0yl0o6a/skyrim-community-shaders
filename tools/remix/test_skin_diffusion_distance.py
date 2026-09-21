"""Analytic checks of the adapter's spatial estimator, not GPU appearance tests."""

import math
from pathlib import Path
import unittest

import numpy as np

ROOT = Path(__file__).resolve().parents[2]


def radial_density(inverse_length, radius):
    return 0.25 * inverse_length * (np.exp(-radius * inverse_length) +
                                    np.exp(-radius * inverse_length / 3))


def spatial_weight(delta, hit_normal, normal_axis, view_axis, units=69.99125):
    delta = np.asarray(delta)
    radius = np.maximum(np.linalg.norm(delta, axis=-1), 1e-6 * units)
    inverse_length = 200 / units * (1.85 - 0.6 + 7 * abs(0.6 - 0.8)**3)
    pdf = np.zeros_like(radius)
    for axis in (np.array(normal_axis), np.array(view_axis)):
        projected = delta - np.sum(delta * axis, axis=-1)[..., None] * axis
        projected_radius = np.maximum(np.linalg.norm(projected, axis=-1), 1e-6 * units)
        pdf += 0.5 * radial_density(inverse_length, projected_radius) / projected_radius * abs(np.dot(hit_normal, axis))
    return 0.6 * radial_density(inverse_length, radius) / (radius * pdf)


class SkinDiffusionDistanceTests(unittest.TestCase):
    def test_planar_normal_projection_preserves_sdk_weight(self):
        for r in (0.001, 0.1, 1, 10):
            self.assertAlmostEqual(float(spatial_weight([r, 0, 0], [0, 0, 1], [0, 0, 1], [0, 0, 1])), 0.6)

    def test_distant_same_material_surface_is_not_local_skin(self):
        local = spatial_weight([0.1, 0, 0], [0, 0, 1], [0, 0, 1], [0, 0, 1])
        across_body = spatial_weight([0.1, 0, 10], [0, 0, 1], [0, 0, 1], [0, 0, 1])
        self.assertLess(across_body / local, 1e-6)

    def test_spatial_weight_is_unit_invariant(self):
        delta_m = np.array([0.003, 0.001, 0.002])
        expected = spatial_weight(delta_m, [0, 0, 1], [0, 0, 1], [0, 0.6, 0.8], units=1)
        for units in (69.99125, 100, 1000):
            self.assertAlmostEqual(float(spatial_weight(delta_m * units, [0, 0, 1], [0, 0, 1], [0, 0.6, 0.8], units)), float(expected))

    def test_two_axis_pdf_normalizes_on_a_plane(self):
        # Monte Carlo integral on an unbounded plane, including an oblique axis.
        rng = np.random.default_rng(816)
        count = 200000
        inverse_length = 200 / 69.99125 * (1.85 - 0.6 + 7 * abs(0.6 - 0.8)**3)
        u = rng.random(count)
        radius = np.where(u < 0.25, -np.log(np.maximum(u * 4, 1e-15)) / inverse_length,
                          -3 * np.log(np.maximum((u - 0.25) / 0.75, 1e-15)) / inverse_length)
        phi = rng.random(count) * 2 * math.pi
        use_oblique = rng.random(count) < 0.5
        delta = np.zeros((count, 3))
        delta[:, 0] = radius * np.cos(phi)
        delta[:, 1] = radius * np.sin(phi) / np.where(use_oblique, 0.8, 1.0)
        weights = spatial_weight(delta, [0, 0, 1], [0, 0, 1], [0, 0.6, 0.8])
        self.assertAlmostEqual(float(np.mean(weights)), 0.6, delta=0.003)

    def test_shader_reweights_actual_hit_and_constructs_disk_basis(self):
        source = (ROOT / '.research/dxvk-remix/src/dxvk/shaders/rtx/algorithm/rtxcr/rtxcr_material.slangh').read_text()
        self.assertIn('calcOrthonormalBasis(diskNormalWorldSpace, diskTangent, diskBitangent)', source)
        self.assertIn('subsurfaceSample.bssrdfWeight = evalSssSurfaceWeight(', source)
        self.assertIn('length(sampleDelta)', source)
        self.assertIn('0.5f * (normalPdf + viewPdf)', source)
        self.assertIn('surfaceDistance > maxSampleRadius', source)
        self.assertIn('radius * surfacePdf', source)


if __name__ == '__main__':
    unittest.main()
