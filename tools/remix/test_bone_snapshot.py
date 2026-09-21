import unittest
from pathlib import Path
from AnalyzeFacialAlignment import rotation_delta


class BoneSnapshotTests(unittest.TestCase):
    def test_attachment_pose_cannot_take_static_probe_shortcut(self):
        source = (Path(__file__).resolve().parents[2] / 'src/RemixScene.cpp').read_text()
        capture = source[source.index('static void Capture('):source.index('void RecordAudit(')]
        self.assertIn('const bool animatedAttachment = attachmentAuditTransforms.contains(geometry);', capture)
        self.assertLess(capture.index('cached->second.probeEligible = false;'), capture.index('cached->second.probeEligible)'))
        self.assertIn('if (shape && !animatedAttachment && !water', capture)

    def test_native_snapshot_retains_meshes_until_consumed(self):
        source = (Path(__file__).resolve().parents[2] / 'src/RemixScene.cpp').read_text()
        helper = source[source.index('void CaptureViewModelPose()'):source.index('static void Capture(')]
        self.assertLess(helper.index('nativeViewModelOwners.emplace_back(geometry)'), helper.index('SnapshotBones(geometries, nativeViewModelBones)'))
        submit = source[source.index('for (const auto& [geometry, native] : nativeViewModelWorlds)'):]
        self.assertLess(submit.index('!viewModelGeometry.contains(geometry)'), submit.index('geometry->GetGeometryRuntimeData()'))
        discard = source[source.index('void DiscardFrame()'):source.index('static uint32_t SceneLightMode()')]
        for member in ('nativeViewModelOwners', 'nativeViewModelBones', 'nativeViewModelWorlds', 'nativeViewModelAttachments', 'nativeViewModelAnchors'):
            self.assertIn(f'{member}.clear();', discard)
        self.assertIn('nativeViewModelFrame = 0;', discard)

    def test_native_pose_audit_is_opt_in_and_frame_scoped(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / 'src/RemixScene.cpp').read_text()
        helper = source[source.index('void AuditNativeViewModelPose()'):source.index('static void SnapshotBones(')]
        self.assertIn('if (!auditEnabled.load(std::memory_order_relaxed)) return;', helper)
        self.assertIn('nativeViewModelPoseFrame = globals::state->frameCount;', helper)
        self.assertIn('const auto value = *source;', helper)
        self.assertIn('nativeViewModelPoseFrame == sample.frame', source)
        native = (root / 'src/RemixNativeRender.cpp').read_text()
        hook = native[native.index('struct ViewModelCameraCall'):native.index('struct MatchedWorldCameraCacheCall')]
        self.assertLess(hook.index('func(state, camera, flags);'), hook.index('RemixScene::CaptureViewModelPose();'))

    def test_rotation_comparison_ignores_local_translation(self):
        pose = dict(world=[0, 0, 0, 1], rotation=[1, 0, 0, 0, 1, 0, 0, 0, 1],
                    bones=[[1, 0, 0, 2, 0, 1, 0, 3, 0, 0, 1, 4]])
        other = dict(pose, bones=[[1, 0, 0, 8, 0, 1, 0, 9, 0, 0, 1, 10]])
        self.assertEqual(rotation_delta(pose, other), 0)
        other['bones'][0][0] = 0.9
        self.assertAlmostEqual(rotation_delta(pose, other), 0.1)

    def test_capture_uses_frame_local_shared_snapshot(self):
        source = (Path(__file__).resolve().parents[2] / 'src/RemixScene.cpp').read_text()
        capture = source[source.index('static void Capture('):source.index('void RecordAudit(')]
        self.assertNotIn('*skin->boneWorldTransforms[bone]', capture)
        self.assertIn('inverse * sampled->second', capture)
        self.assertIn('const auto inverse = instance.world.Invert()', capture)
        submit = source[source.index('bool Submit()'):]
        self.assertLess(submit.index('BoneSnapshot boneSnapshot;'), submit.index('Capture(geometry, boneSnapshot)'))
        helper = source[source.index('static void SnapshotBones('):source.index('static void Capture(')]
        self.assertIn('!snapshot.contains(source)', helper)
        self.assertIn('snapshot.emplace(source, *source)', helper)

    def test_existing_skeletons_sampled_before_discovery(self):
        source = (Path(__file__).resolve().parents[2] / 'src/RemixScene.cpp').read_text()
        submit = source[source.index('bool Submit()'):]
        early = submit.index('SnapshotBones(captureOrder, boneSnapshot);')
        discovery = submit.index('RemixSceneGraph::Gather()')
        late = submit.index('SnapshotBones(captureOrder, boneSnapshot);', early + 1)
        self.assertLess(early, discovery)
        self.assertLess(discovery, late)
        self.assertLess(late, submit.index('Capture(geometry, boneSnapshot)'))
        self.assertEqual(submit.count('BoneSnapshot boneSnapshot;'), 1)

    def test_rigid_viewmodel_uses_shared_bone_attachment(self):
        source = (Path(__file__).resolve().parents[2] / 'src/RemixScene.cpp').read_text()
        helper = source[source.index('static void SnapshotViewModelAttachments('):source.index('static void Capture(')]
        self.assertIn('snapshot.find(skin->boneWorldTransforms[bone])', helper)
        self.assertIn('nodeBones.emplace(skin->bones[bone], &sampled->second)', helper)
        self.assertIn('*anchor->second * localChain', helper)
        self.assertIn('localChain = node->local * localChain;', helper)
        self.assertIn('transforms.contains(geometry)', helper)
        self.assertIn('if (!boneCount && (viewModelGeometry.contains(geometry) || playerBody.contains(geometry)))', source)
        submit = source[source.index('bool Submit()'):]
        self.assertLess(submit.index('attachmentAuditTransforms.clear();'), submit.index('SnapshotViewModelAttachments(boneSnapshot)'))

    def test_third_person_attachments_sample_before_discovery(self):
        source = (Path(__file__).resolve().parents[2] / 'src/RemixScene.cpp').read_text()
        submit = source[source.index('bool Submit()'):]
        call = 'SnapshotViewModelAttachments(boneSnapshot, attachmentAuditTransforms, attachmentAuditAnchors, playerBody);'
        self.assertEqual(submit.count(call), 2)
        self.assertLess(submit.index(call), submit.index('RemixSceneGraph::Gather()'))
        helper = source[source.index('static void SnapshotViewModelAttachments('):source.index('void CaptureViewModelPose()')]
        self.assertEqual(helper.count('for (auto* geometry : geometries)'), 2)

    def test_native_viewmodel_snapshot_restores_camera_translation(self):
        source = (Path(__file__).resolve().parents[2] / 'src/RemixScene.cpp').read_text()
        helper = source[source.index('void CaptureViewModelPose()'):source.index('static void Capture(')]
        self.assertIn('nativeViewModelFrame = globals::state->frameCount;', helper)
        self.assertIn('nativeViewModelBones.clear();', helper)
        self.assertIn('SnapshotBones(geometries, nativeViewModelBones);', helper)
        self.assertIn('SnapshotViewModelAttachments(nativeViewModelBones, nativeViewModelAttachments, nativeViewModelAnchors);', helper)
        submit = source[source.index('bool Submit()'):]
        self.assertIn('nativeViewModelFrame == globals::state->frameCount', submit)
        self.assertIn('!nativeViewModelWorlds.contains(geometry)', submit)
        self.assertIn('restored.translate += viewModelTranslation;', submit)
        self.assertLess(submit.index('boneSnapshot.insert_or_assign(source, restored);'), submit.index('Capture(geometry, boneSnapshot);'))
