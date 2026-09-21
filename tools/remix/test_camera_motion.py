import unittest
from MeasureCameraMotion import compare


class CameraMotionTests(unittest.TestCase):
    def row(self, frame, x):
        return dict(frame=frame, cameraValid=True, nativeCameraPositionsValid=True,
                    eyeWorld=[x, 0, 0], nativeSceneCameraPosition=[x, 0, 0],
                    nativePlayerPosition=[0, 0, 0], nativeCameraOrigin=[100, 0, 0],
                    nativeRelativeEye=[x - 100, 0, 0])

    def test_native_jump_not_restore_error(self):
        result = compare([self.row(10, 1), self.row(11, 111)])
        self.assertEqual(result[1]['submittedStep'], 110)
        self.assertEqual(result[1]['nativeSceneStep'], 110)
        self.assertEqual(result[1]['playerStep'], 0)
        self.assertEqual(result[1]['restoreError'], 0)

    def test_stale_submission_detected(self):
        row = self.row(10, 50)
        row['eyeWorld'] = [20, 0, 0]
        self.assertEqual(compare([row])[0]['restoreError'], 30)

    def test_missing_or_nonconsecutive_rejected(self):
        row = self.row(10, 0)
        row['nativeCameraPositionsValid'] = False
        with self.assertRaises(ValueError):
            compare([row])
        with self.assertRaises(ValueError):
            compare([self.row(10, 0), self.row(12, 0)])
