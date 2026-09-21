import unittest

import numpy as np

from CompareSkinBuffers import authored_skin_normals, metrics, skin_mask


class SkinBufferTests(unittest.TestCase):
    def test_authored_normal_mask_and_decode(self):
        values = np.array([[[.5, .5, 1, 1], [0, 0, 0, 0], [1, .5, .5, 1]]])
        mask, normal, length = authored_skin_normals(values)
        np.testing.assert_array_equal(mask, [[True, False, True]])
        np.testing.assert_allclose(normal[mask], [[0, 0, 1], [1, 0, 0]])
        np.testing.assert_allclose(length[mask], [1, 1])

    def test_authored_invalid_selected_normal_rejected(self):
        for value in ([[.5, .5, .5, 1]], [[np.nan, .5, 1, 1]]):
            with self.assertRaises(ValueError):
                authored_skin_normals(np.array([value]))

    def test_authored_nonunit_normal_remains_in_mask(self):
        mask, normal, length = authored_skin_normals(np.array([[[1., 1., 1., 1.]]]))
        self.assertTrue(mask[0, 0])
        self.assertAlmostEqual(length[0, 0], np.sqrt(3))
        np.testing.assert_allclose(normal[0, 0], np.ones(3) / np.sqrt(3))

    def test_mask_excludes_other_materials_and_mixed_edges(self):
        colors = np.array([[[0, 1, 0], [1, 0, 0], [.5, .5, 0], [0, 0, 0]]])
        np.testing.assert_array_equal(skin_mask(colors), [[True, False, False, False]])

    def test_metrics_keep_bad_normal_and_depth_samples(self):
        normals = np.array([[[0., 0., 1.], [0., 0., 1.]]])
        target = np.array([[[0., 0., 1.], [0., 1., 0.]]])
        depth = np.array([[20., 20.]])
        colors = np.full((1, 2, 3), .5)
        result = metrics(np.ones((1, 2), bool), normals, target, depth, depth + [[0, 10]], colors, colors)
        self.assertEqual(result['pixels'], 2)
        self.assertEqual(result['normalOver30DegreeFraction'], .5)
        self.assertEqual(result['normalAngleP50P90P99Max'][-1], 90)
        self.assertEqual(result['absoluteDepthP50P90P99'][0], 5)
        self.assertEqual(result['rawNativeAlbedoMAE'], [0, 0, 0])

    def test_nonfinite_samples_rejected(self):
        normal = np.array([[[0., 0., 1.]]])
        with self.assertRaises(ValueError):
            metrics(np.ones((1, 1), bool), normal, normal, np.array([[1.]]), np.array([[np.nan]]), normal, normal)

    def test_empty_subset_not_a_pass(self):
        value = np.zeros((1, 1, 3))
        self.assertEqual(metrics(np.zeros((1, 1), bool), value, value, value[..., 0], value[..., 0], value, value), {'pixels': 0})

    def test_known_scale_diagnostic_preserves_raw_error(self):
        normal = np.array([[[0., 0., 1.]]])
        color = np.full((1, 1, 3), .5)
        depth = np.ones((1, 1))
        target = color ** 2.2 * 1.5
        result = metrics(depth.astype(bool), normal, normal, depth, depth, color, target, 1.5)
        self.assertGreater(result['pow22NativeAlbedoMAE'][0], .1)
        np.testing.assert_allclose(result['configuredScaleDiagnostic']['pow22NativeAlbedoMAE'], 0, atol=1e-15)
        self.assertEqual(result['configuredScaleDiagnostic']['scale'], 1.5)

    def test_scale_diagnostic_reports_unrecoverable_saturation(self):
        normal = np.array([[[0., 0., 1.]]])
        color = np.full((1, 1, 3), .9)
        depth = np.ones((1, 1))
        result = metrics(depth.astype(bool), normal, normal, depth, depth, color, np.ones_like(color), 2)
        self.assertEqual(result['configuredScaleDiagnostic']['targetClippedChannelFraction'], [1, 1, 1])
        self.assertGreater(result['configuredScaleDiagnostic']['pow22NativeAlbedoMAE'][0], .2)

    def test_scale_is_never_inferred(self):
        normal = np.array([[[0., 0., 1.]]])
        depth = np.ones((1, 1))
        result = metrics(depth.astype(bool), normal, normal, depth, depth, normal, normal)
        self.assertNotIn('configuredScaleDiagnostic', result)

    def test_invalid_scales_rejected_even_on_empty_mask(self):
        value = np.zeros((1, 1, 3))
        for scale in (0, -1, np.nan, np.inf):
            with self.subTest(scale=scale), self.assertRaises(ValueError):
                metrics(np.zeros((1, 1), bool), value, value, value[..., 0], value[..., 0], value, value, scale)


if __name__ == '__main__':
    unittest.main()
