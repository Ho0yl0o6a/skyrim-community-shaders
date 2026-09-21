"""Float32 reference/source contracts, not GPU shader execution tests."""

from pathlib import Path
import unittest

import numpy as np

from CompareGrazingBuffers import view_directions


def normalize(value):
    return value / np.linalg.norm(value, axis=-1, keepdims=True)


def corrected_normal(geometry, shading, incident, guard=True):
    g, n, v = (np.asarray(value, dtype=np.float32) for value in (geometry, shading, incident))
    reflected = 2 * np.sum(n * v, axis=-1, keepdims=True) * n - v
    gv = np.maximum(0, np.sum(g * v, axis=-1))
    margin = np.minimum(.98 * gv, .01)
    keep = np.sum(g * reflected, axis=-1) >= margin
    if guard:
        keep &= np.sum(n * v, axis=-1) > 0
    tangent_vector = n - g * np.sum(n * g, axis=-1, keepdims=True)
    length = np.linalg.norm(tangent_vector, axis=-1)
    tangent = tangent_vector / np.maximum(length[..., None], 1e-8)
    tv = np.sum(tangent * v, axis=-1)
    root = np.sqrt(np.maximum(0, tv * tv + gv * gv - margin * margin))
    denominator = np.where(tv >= 0, gv + margin, root - tv)
    slope = np.where(tv >= 0, tv + root, gv - margin) / np.maximum(denominator, 1e-8)
    result = normalize(g + slope[..., None] * tangent)
    reflection = 2 * np.sum(result * v, axis=-1, keepdims=True) * result - v
    fallback = ((length <= 1e-8) | (denominator <= 1e-8) |
                (np.sum(result * v, axis=-1) <= 0) | (np.sum(g * reflection, axis=-1) < 0))
    result = np.where(fallback[..., None], g, result)
    return np.where(keep[..., None], n, result)


class GrazingNormalsTest(unittest.TestCase):
    def test_authored_native_gate_preserves_positive_normals_only(self):
        rng = np.random.default_rng(721)
        n = normalize(rng.normal(size=(20000, 3)).astype(np.float32))
        g = np.array([0, 0, 1], dtype=np.float32)
        v = normalize(np.array([1, 0, .01], dtype=np.float32))
        keep = n @ v > 0
        result = np.where(keep[:, None], n, corrected_normal(g, n, v))
        np.testing.assert_array_equal(result[keep], n[keep])
        self.assertGreater(float((result @ v).min()), 0)
        self.assertTrue(np.isfinite(result).all())

    def test_reflection_sign_ambiguity_reproduction(self):
        g = np.array([0, 0, 1], dtype=np.float32)
        v = normalize(np.array([1, 0, .1], dtype=np.float32))
        n = -g
        self.assertLess(np.dot(corrected_normal(g, n, v, guard=False), v), 0)
        self.assertGreater(np.dot(corrected_normal(g, n, v), v), 0)

    def test_full_sphere_normals_remain_valid(self):
        rng = np.random.default_rng(719)
        n = normalize(rng.normal(size=(200000, 3)).astype(np.float32))
        v = rng.normal(size=(200000, 3)).astype(np.float32)
        v[:, 2] = np.maximum(np.abs(v[:, 2]), 1e-6)
        v = normalize(v)
        g = np.broadcast_to(np.array([0, 0, 1], dtype=np.float32), n.shape)
        result = corrected_normal(g, n, v)
        nv = np.sum(result * v, axis=-1)
        reflection = 2 * nv[:, None] * result - v
        self.assertTrue(np.isfinite(result).all())
        self.assertGreater(float(nv.min()), 0)
        self.assertGreaterEqual(float(reflection[:, 2].min()), -1e-6)
        np.testing.assert_allclose(np.linalg.norm(result, axis=-1), 1, atol=2e-7)

    def test_view_facing_inputs_unchanged_by_guard(self):
        rng = np.random.default_rng(720)
        n = normalize(rng.normal(size=(10000, 3)).astype(np.float32))
        v = normalize(np.array([1, 0, .01], dtype=np.float32))
        n = n[n @ v > 0]
        g = np.array([0, 0, 1], dtype=np.float32)
        np.testing.assert_array_equal(corrected_normal(g, n, v), corrected_normal(g, n, v, False))

    def test_normal_incidence_valid_normal_preserved(self):
        g = np.array([0, 0, 1], dtype=np.float32)
        np.testing.assert_array_equal(corrected_normal(g, g, g), g)

    def test_camera_ray_points_toward_viewer(self):
        identity = np.eye(4).reshape(-1).tolist()
        runtime = {'projectionToViewJittered': identity, 'viewToWorld': identity}
        np.testing.assert_array_equal(view_directions(runtime, (1, 1)), [[[0, 0, -1]]])

    def test_shader_guard_contract(self):
        source = (Path(__file__).resolve().parents[2] / '.research/dxvk-remix/src/dxvk/shaders/rtx/utility/math.slangh').read_text()
        self.assertIn('if (dot(shadingNormal, incidentDirection) > 0.0f &&\n'
                      '      dot(geometryNormal, reflectedDirection) >= minGeometryNormalDotReflection)', source)


if __name__ == '__main__':
    unittest.main()
