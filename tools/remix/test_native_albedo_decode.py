"""Source contract and arithmetic for native UNORM diffuse imports.

Not a substitute for native/Remix buffer parity or SRGB-view coverage.
"""
import math
from pathlib import Path
import unittest

SHADER = Path(__file__).resolve().parents[2] / (
    '.research/dxvk-remix/src/dxvk/shaders/rtx/concept/surface_material/opaque_surface_material_interaction.slangh'
)


class NativeAlbedoDecodeTests(unittest.TestCase):
    def test_sampling_does_not_decode_before_texture_operations(self):
        body = SHADER.read_text().split('  if (albedoOpacityLoaded)\n', 1)[1]
        sampling = body.split('  vec4 tFactor', 1)[0]
        self.assertNotIn('gammaToLinear(albedo)', sampling)

    def test_linear_scale_after_single_decode_and_not_in_grass(self):
        body = SHADER.read_text().split('  const bool nativeGrass =', 1)[1].split('  // Load Normal', 1)[0]
        self.assertEqual(body.count('gammaToLinear(albedo)'), 1)
        self.assertLess(body.index('gammaToLinear(albedo)'), body.index('albedo *= cb.opaqueMaterialArgs.nativeAlbedoScale'))
        self.assertIn('!nativeEffect && albedoOpacityLoaded', body)
        grass = body.split('  else\n', 1)[0]
        self.assertNotIn('nativeAlbedoScale', grass)

    def test_duplicate_decode_darkens_midtones(self):
        for sample in (0.1, 0.25, 0.5, 0.75, 0.9):
            linear = math.pow(sample, 2.2)
            duplicate = math.pow(linear, 2.2)
            self.assertLess(duplicate, linear)
        self.assertAlmostEqual(math.pow(0.5, 2.2), 0.2176376408)


if __name__ == '__main__':
    unittest.main()
