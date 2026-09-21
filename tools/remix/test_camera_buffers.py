import unittest

import numpy as np

from CameraBuffers import coincident_pixel_centres, native_pixel_coordinates


class CameraBuffersTests(unittest.TestCase):
    def setUp(self):
        self.projection = np.array([[1.2, 0, 0, 0], [0, 2.1, 0, 0],
                                    [0, 0, 1.00004, -15], [0, 0, 1, 0.]])
        self.native = dict(shadowView=np.eye(4).T.ravel().tolist(),
                           shadowProjection=self.projection.T.ravel().tolist())
        self.runtime = dict(schema=1, source="uploaded-raytrace-args", matrixLayout="column-major",
                            resolution=[8, 6], depthExtent=[8, 6], worldToView=self.native["shadowView"],
                            viewToProjectionJittered=self.projection.T.ravel().tolist(),
                            projectionToViewJittered=np.linalg.inv(self.projection).T.ravel().tolist())

    def test_equal_camera_pixel_centres(self):
        x, y = native_pixel_coordinates(self.native, self.runtime, (6, 8), (6, 8))
        yy, xx = np.indices((6, 8))
        np.testing.assert_allclose(x, xx, atol=2e-6)
        np.testing.assert_allclose(y, yy, atol=2e-6)
        self.assertTrue(coincident_pixel_centres(x, y, (6, 8)))

    def test_sample_gate_rejects_resolution_jitter_and_invalid_coordinates(self):
        yy, xx = np.indices((6, 8), dtype=float)
        self.assertFalse(coincident_pixel_centres(xx, yy, (9, 12)))
        self.assertFalse(coincident_pixel_centres(xx + .01, yy, (6, 8)))
        xx[2, 2] = np.nan
        self.assertFalse(coincident_pixel_centres(xx, yy, (6, 8)))

    def test_resolution_change_preserves_centres(self):
        x, y = native_pixel_coordinates(self.native, self.runtime, (6, 8), (9, 12))
        yy, xx = np.indices((6, 8))
        np.testing.assert_allclose(x, (xx + .5) * 1.5 - .5, atol=2e-6)
        np.testing.assert_allclose(y, (yy + .5) * 1.5 - .5, atol=2e-6)

    def test_native_rebasing_origin(self):
        self.native["shadowOrigin"] = [13000, -19988.578125, 650]
        world_view = np.eye(4)
        world_view[:3, 3] = [-13000, 19988.578125, -650]
        self.runtime["worldToView"] = [float(format(v, '.9g')) for v in world_view.T.ravel()]
        x, y = native_pixel_coordinates(self.native, self.runtime, (6, 8), (6, 8))
        yy, xx = np.indices((6, 8))
        np.testing.assert_allclose(x, xx, atol=2e-6)
        np.testing.assert_allclose(y, yy, atol=2e-6)

    def test_jitter_sign_and_pixel_units(self):
        self.projection[0, 2] = .1
        self.projection[1, 2] = -.2
        self.native["shadowProjection"] = self.projection.T.ravel().tolist()
        x, y = native_pixel_coordinates(self.native, self.runtime, (6, 8), (6, 8))
        yy, xx = np.indices((6, 8))
        np.testing.assert_allclose(x, xx + .4, atol=2e-6)
        np.testing.assert_allclose(y, yy + .6, atol=2e-6)

    def test_rejects_wrong_resolution_inverse_layout_and_view(self):
        for key, value in (("resolution", [7, 6]), ("matrixLayout", "row-major"),
                           ("projectionToViewJittered", np.eye(4).ravel().tolist()),
                           ("worldToView", (np.eye(4) * 2).ravel().tolist())):
            with self.subTest(key=key), self.assertRaises(ValueError):
                native_pixel_coordinates(self.native, dict(self.runtime, **{key: value}), (6, 8), (6, 8))


if __name__ == "__main__":
    unittest.main()
