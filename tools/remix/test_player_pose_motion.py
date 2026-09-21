import unittest
import numpy as np
from MeasurePlayerPoseMotion import world_bone_origins, world_bone_rotations


class PlayerPoseMotion(unittest.TestCase):
    def test_rigid_pose_uses_object_transform(self):
        pose = dict(rotation=[1,0,0,0,1,0,0,0,1], world=[10,20,30,2], bones=[])
        np.testing.assert_array_equal(world_bone_origins(pose), [[10,20,30]])
        np.testing.assert_array_equal(world_bone_rotations(pose), [np.eye(3)*2])

    def test_root_and_bone_translation_cancel(self):
        pose = dict(rotation=[1,0,0,0,1,0,0,0,1], world=[100,200,300,2],
                    bones=[[1,0,0,-50,0,1,0,-100,0,0,1,-150]])
        np.testing.assert_array_equal(world_bone_origins(pose), [[0,0,0]])

    def test_rotation_precedes_world_translation(self):
        pose = dict(rotation=[0,-1,0,1,0,0,0,0,1], world=[10,20,30,1],
                    bones=[[1,0,0,2,0,1,0,3,0,0,1,4]])
        np.testing.assert_array_equal(world_bone_origins(pose), [[7,22,34]])

    def test_world_scale_and_rotation_cancel_bone_inverse(self):
        pose = dict(rotation=[0,-1,0,1,0,0,0,0,1], world=[10,20,30,2],
                    bones=[[0,.5,0,0,-.5,0,0,0,0,0,.5,0]])
        np.testing.assert_array_equal(world_bone_rotations(pose), [np.eye(3)])


if __name__ == '__main__':
    unittest.main()
