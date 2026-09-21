import unittest
from AnalyzeFrameSequence import validate_records


class FrameSequenceTests(unittest.TestCase):
    def fixture(self):
        return [dict(index=i, count=4, saved=True, frame=100+i, captureTimeUs=1000+i*10,
                     pid=2, width=1920, height=1080, format=28, source='test') for i in range(4)]

    def test_complete(self):
        validate_records(self.fixture())

    def test_missing(self):
        with self.assertRaises(ValueError):
            validate_records(self.fixture()[:-1])

    def test_mutations_rejected(self):
        for key, value in [('frame', 200), ('captureTimeUs', 1), ('pid', 3), ('width', 500),
                           ('height', 0), ('format', 10), ('saved', False), ('count', 3), ('index', 0)]:
            with self.subTest(key=key):
                rows = self.fixture()
                rows[2][key] = value
                with self.assertRaises(ValueError):
                    validate_records(rows)


if __name__ == '__main__':
    unittest.main()
