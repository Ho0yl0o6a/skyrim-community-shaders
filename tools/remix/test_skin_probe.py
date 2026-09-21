import unittest
from CheckSkinProbe import validate


class SkinProbeTests(unittest.TestCase):
    def fixture(self):
        return '\n'.join(f'[CSRemix.skinProbe] sample={i} frame={10+i*5} key=2 boneHash={100+i} '
                         'vertices=32 maxPositionError=0.00001 maxNormalError=0.000001 '
                         'maxMovement=0.02 nonFinite=0' for i in range(12))

    def test_complete(self):
        self.assertTrue(validate(self.fixture())['passed'])

    def test_face_complete(self):
        records = '\n'.join(f'[CSRemix.skinProbe] sample={i} frame={10+i*5} key=2 boneHash={100+i} '
                            'vertices=898 faceProbe=1 maxPositionError=0.00001 maxNormalError=0.000001 '
                            'maxMovement=0.02 nonFinite=0' for i in range(64))
        self.assertTrue(validate(records, face=True)['passed'])
        with self.assertRaises(ValueError):
            validate(records)

    def test_body_not_face(self):
        with self.assertRaises(ValueError):
            validate(self.fixture(), face=True)

    def test_selected_face_class(self):
        records = '\n'.join(f'[CSRemix.skinProbe] sample={i} frame={10+i*5} key=2 boneHash={100+i} '
                            'vertices=250 faceProbe=1 maxPositionError=0.00001 maxNormalError=0.000001 '
                            'maxMovement=0.02 nonFinite=0' for i in range(64))
        self.assertTrue(validate(records, face=True, vertices=250)['passed'])
        with self.assertRaises(ValueError):
            validate(records, face=True)
        for count in (0, 4097):
            with self.assertRaises(ValueError):
                validate(records, face=True, vertices=count)

    def test_single_bad_sample_not_hidden(self):
        with self.assertRaises(ValueError):
            validate(self.fixture().replace('maxPositionError=0.00001', 'maxPositionError=2', 1))

    def test_missing(self):
        with self.assertRaises(ValueError):
            validate('\n'.join(self.fixture().splitlines()[:-1]))

    def test_bad_normal(self):
        with self.assertRaises(ValueError):
            validate(self.fixture().replace('maxNormalError=0.000001', 'maxNormalError=106', 1))

    def test_non_finite_counter(self):
        with self.assertRaises(ValueError):
            validate(self.fixture().replace('nonFinite=0', 'nonFinite=1', 1))

    def test_nan(self):
        with self.assertRaises(ValueError):
            validate(self.fixture().replace('maxNormalError=0.000001', 'maxNormalError=nan', 1))

    def test_duplicate_frame(self):
        with self.assertRaises(ValueError):
            validate(self.fixture().replace('frame=15', 'frame=10'))

    def test_no_motion(self):
        with self.assertRaises(ValueError):
            validate(self.fixture().replace('maxMovement=0.02', 'maxMovement=0'))


if __name__ == '__main__':
    unittest.main()
