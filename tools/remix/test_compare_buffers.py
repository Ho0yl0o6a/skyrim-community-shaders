"""Synthetic selection tests, not renderer fidelity tests."""

import unittest

import numpy as np

from CompareBuffers import normal_agreement_diagnostics, validate_capture_pair
from NormalBuffers import angle_degrees


class AgreementDiagnosticsTests(unittest.TestCase):
    def setUp(self):
        self.native = np.zeros((5, 5, 3))
        self.native[..., 2] = 1
        self.remix = self.native.copy()
        self.eligible = np.ones((5, 5), dtype=bool)
        self.depth_error = np.zeros((5, 5))

    def report(self):
        return normal_agreement_diagnostics(
            self.native, self.remix, angle_degrees(self.native, self.remix),
            self.eligible, self.depth_error)

    def test_identical_constant_normals(self):
        report = self.report()
        for gate in report["absoluteDepthGateDiagnostics"]:
            self.assertEqual(gate["samples"], 25)
            self.assertEqual(gate["anglePercentiles"], [0, 0, 0])
            self.assertEqual(gate["over30DegreeFraction"], 0)
        self.assertEqual(report["continuousNormalDiagnostic"]["samples"], 25)

    def test_empty_selection_is_not_zero_error(self):
        self.eligible[:] = False
        report = self.report()
        for gate in report["absoluteDepthGateDiagnostics"]:
            self.assertEqual(gate["samples"], 0)
            self.assertIsNone(gate["anglePercentiles"])
            self.assertIsNone(gate["over30DegreeFraction"])
        self.assertIsNone(report["continuousNormalDiagnostic"]["anglePercentiles"])

    def test_strict_depth_thresholds_and_invalid_depth(self):
        self.depth_error[:] = np.inf
        self.depth_error[0] = [0.05, 0.1, 0.5, 2.0, 10.0]
        self.depth_error[1, 0] = np.nan
        self.eligible[0, 0] = False
        gates = self.report()["absoluteDepthGateDiagnostics"]
        self.assertEqual([gate["samples"] for gate in gates], [3, 2, 1, 0])

    def test_boundary_exclusion_in_either_image(self):
        for target in (self.native, self.remix):
            with self.subTest(target="native" if target is self.native else "remix"):
                target[2, 2] = [1, 0, 0]
                report = self.report()
                self.assertEqual(report["continuousNormalDiagnostic"]["samples"], 16)
                # The broader depth probe must still expose the outlier.
                self.assertEqual(report["absoluteDepthGateDiagnostics"][0]["samples"], 25)
                self.assertEqual(report["absoluteDepthGateDiagnostics"][0]["over30DegreeFraction"], 1 / 25)
                target[2, 2] = [0, 0, 1]


class CapturePairTests(unittest.TestCase):
    def setUp(self):
        self.native = dict(pairedCapture=True, nativeReference=True, nativePreparation=True, pid=7, request=12, frame=100)
        self.remix = dict(pairedCapture=True, nativeReference=False, pid=7, request=12, frame=100,
                          queued=True, pairedNativeCaptured=True, nativePreparation=True)

    def test_valid_and_legacy(self):
        self.assertTrue(validate_capture_pair(self.native, self.remix))
        self.assertFalse(validate_capture_pair({}, {}))

    def test_frame_and_identity_mismatch(self):
        for key in ("pid", "request", "frame"):
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "frame/identity"):
                validate_capture_pair(self.native, dict(self.remix, **{key: -1}))

    def test_incomplete_pair(self):
        for key in ("pairedCapture", "pairedNativeCaptured", "queued", "nativePreparation"):
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "Incomplete"):
                validate_capture_pair(self.native, dict(self.remix, **{key: False}))


if __name__ == "__main__":
    unittest.main()
