"""CPU/source regression guards; not GPU execution or visual acceptance."""
from pathlib import Path
import unittest

SHADER = Path(__file__).resolve().parents[2] / '.research/dxvk-remix/src/dxvk/shaders/rtx/pass/composite/composite_alpha_blend.comp.slang'


class AlphaNeighborSafety(unittest.TestCase):
    def test_all_lanes_reach_barriers(self):
        source = SHADER.read_text()
        self.assertNotIn('return;', source)
        self.assertNotIn('if (surface.isValid())', source)
        self.assertIn('const bool validSurface = inBounds && surface.isValid();', source)
        self.assertIn('bool found = !validSurface;', source)
        self.assertIn('if (validSurface && found)', source)
        self.assertEqual(source.count('GroupMemoryBarrierWithGroupSync();'), 3)

    def test_shared_neighbor_jitter_is_bounded(self):
        source = SHADER.read_text()
        jitter = source.index('neighborPixel += int2(offset2')
        clamp = source.index('neighborPixel = clamp(neighborPixel, int2(0), int2(cb.camera.resolution) - 1);')
        barrier = source.index('GroupMemoryBarrierWithGroupSync();', clamp)
        self.assertLess(jitter, clamp)
        self.assertLess(clamp, barrier)
        for width, height in ((1, 1), (17, 9), (1920, 1080)):
            for x in (0, width - 1):
                for y in (0, height - 1):
                    for dx, dy in ((-60, -60), (60, 60), (-60, 60), (60, -60)):
                        nx, ny = max(0, min(width - 1, x + dx)), max(0, min(height - 1, y + dy))
                        self.assertTrue(0 <= nx < width and 0 <= ny < height)

    def test_partial_workgroups_use_safe_load_and_guarded_store(self):
        source = SHADER.read_text()
        self.assertIn('const uint2 pixel = min(thread_id, cb.camera.resolution - 1u);', source)
        self.assertIn('AlphaBlendGBuffer[pixel]', source)
        self.assertIn('if (inBounds)\n  {\n    AlphaBlendRadiance[thread_id.xy]', source)


if __name__ == '__main__':
    unittest.main()
