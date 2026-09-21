"""Source safety contracts, not execution tests or proof of skin appearance."""

from pathlib import Path
import unittest


SOURCE = Path(__file__).resolve().parents[2] / '.research/dxvk-remix/src/dxvk/shaders/rtx/algorithm/integrator_direct.slangh'


class SkinInvalidLightTests(unittest.TestCase):
    def test_diffusion_only_does_not_read_absent_light(self):
        source = SOURCE.read_text()
        body = source.split('void evalNEEPrimary(', 1)[1].split('void calculateRussianRouletteOnFirstBounce(', 1)[0]
        guard = body.split('if (!isValidLightSample)', 1)[1].split('// NV-DXVK end', 1)[0]
        self.assertIn('diffuseLobeRadiance += throughput * vec3(sssDiffusionProfileResult.scatteringWeight);', guard)
        self.assertIn('return;', guard)
        self.assertNotIn('lightSample.', guard)
        self.assertNotIn('specularLobeRadiance', guard)
        self.assertLess(body.index('sssDiffusionProfileResult = evalSssDiffusionProfile('), body.index('if (!isValidLightSample)'))
        self.assertLess(body.index('if (!isValidLightSample)'), body.index('SurfaceMaterialInteractionSplitWeight emptyWeight;'))

    def test_absent_sample_arguments_and_visibility_are_initialized(self):
        source = SOURCE.read_text()
        self.assertIn('VisibilityResult occludedVisibility = (VisibilityResult) 0;', source)
        self.assertIn('occludedVisibility.hasOpaqueHit = true;', source)
        self.assertIn('occludedVisibility.blockerSurfaceIndex = SURFACE_INDEX_INVALID;', source)
        self.assertIn('LightSample lightSample = (LightSample) 0;', source)
        self.assertIn('float inverseSelectionPdf = 0.0f;', source)
        # Diffusion must not depend on the surface-light candidate being valid.
        self.assertIn('if (rtxdiLightSampleValid || risLightSampleValid || isDiffusionProfileSss)', source)


if __name__ == '__main__':
    unittest.main()
