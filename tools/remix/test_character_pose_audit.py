import unittest
from AnalyzeCharacterPoses import join_frames


class CharacterPoseAudit(unittest.TestCase):
    def test_change_and_missing_submission(self):
        pose = {'geometry': 12, 'name': 'beard', 'submitted': True, 'morphHash': 4}
        audit = {'frames': [{'frame': 5, 'ready': True, 'poses': [pose]},
                            {'frame': 6, 'ready': True, 'poses': [dict(pose, submitted=False, morphHash=8)]}]}
        rows = join_frames([{'frame': 5}, {'frame': 6}], audit)
        self.assertEqual(rows[1]['changes'][0]['fields'], ['submitted', 'morphHash'])
        self.assertEqual(rows[1]['unsubmitted'], ['beard'])

    def test_missing_frame_rejected(self):
        with self.assertRaises(ValueError):
            join_frames([{'frame': 5}], {'frames': []})

    def test_empty_pose_rejected(self):
        with self.assertRaises(ValueError):
            join_frames([{'frame': 5}], {'frames': [{'frame': 5, 'ready': True, 'poses': []}]})

    def test_duplicate_frame_rejected(self):
        with self.assertRaises(ValueError):
            join_frames([{'frame': 5}], {'frames': [{'frame': 5}, {'frame': 5}]})
