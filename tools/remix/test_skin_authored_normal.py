"""Diagnostic source contracts; GPU/native parity is measured separately."""

from pathlib import Path
import unittest


SHADERS = Path(__file__).resolve().parents[2] / '.research/dxvk-remix/src/dxvk/shaders/rtx'


class SkinAuthoredNormalTests(unittest.TestCase):
    def test_debug_view_uses_skin_normal_sampler_and_animated_basis(self):
        body = (SHADERS / 'algorithm/geometry_resolver.slangh').read_text().split(
            'case DEBUG_VIEW_SKIN_AUTHORED_NORMAL:', 1)[1].split('// NV-DXVK end', 1)[0]
        self.assertIn('surface.modelSpaceNormals', body)
        self.assertIn('isSubsurfaceDiffusionProfileMaterial', body)
        self.assertIn('skinMaterial.normalTextureIndex', body)
        self.assertIn('skinMaterial.samplerIndex, surfaceInteraction, skinNormalSample', body)
        self.assertIn('skinNormalSample.xzy * 2.0f - 1.0f', body)
        for axis in 'XYZ':
            self.assertIn('surfaceInteraction.modelNormal' + axis, body)
        self.assertNotIn('getBentNormal', body)
        self.assertIn('vec4 authoredNormalColor = vec4(0.0f);', body)
        self.assertIn('vec4(authoredNormal * 0.5f + 0.5f, 1.0f)', body)

    def test_native_msn_preserves_only_view_facing_authored_normal(self):
        source = (SHADERS / 'concept/surface_material/opaque_surface_material_interaction.slangh').read_text()
        self.assertNotIn('DEBUG_VIEW_SKIN_AUTHORED_NORMAL', source)
        self.assertIn('const bool nativeModelNormal = normalLoaded && surface.modelSpaceNormals;', source)
        self.assertIn('const bool keepNativeNormal = (nativeGrassNormal || nativeModelNormal) &&\n    dot(normal, minimalRayInteraction.viewDirection) > 0.0f;', source)
        self.assertIn('shadingNormal = keepNativeNormal ? normal :', source)
        self.assertIn('getBentNormal(surfaceInteraction.geometryNormal, normal, -minimalRayInteraction.viewDirection)', source)


if __name__ == '__main__':
    unittest.main()
