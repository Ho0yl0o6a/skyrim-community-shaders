"""Dimensional regressions and source contracts; not a GPU appearance test."""

import math
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SkinUnitsTests(unittest.TestCase):
    def test_optical_depth_is_invariant_under_world_unit_change(self):
        radius_m = 0.01 * 0.5
        thickness_m = 0.003
        for units_per_meter in (1.0, 69.99125, 100.0, 1000.0):
            sigma_world = 1.0 / (radius_m * units_per_meter)
            thickness_world = thickness_m * units_per_meter
            self.assertAlmostEqual(math.exp(-sigma_world * thickness_world),
                                   math.exp(-thickness_m / radius_m))

    def test_sample_radius_scales_with_geometry(self):
        # Same exponential branch used by RTXCR_SampleBurleyProfileMIS.
        for units_per_meter in (1.0, 69.99125, 100.0, 1000.0):
            sigma_m = 1.0 / 0.005
            sample_m = -3.0 * math.log(0.37) / (sigma_m * 1.4)
            sample_world = -3.0 * math.log(0.37) / ((sigma_m / units_per_meter) * 1.4)
            self.assertAlmostEqual(sample_world / units_per_meter, sample_m)

    def test_single_scattering_integration_weight_is_unit_invariant(self):
        for units_per_meter in (1.0, 69.99125, 100.0, 1000.0):
            sigma_s_m, step_m = 160.0, 0.002
            self.assertAlmostEqual((sigma_s_m / units_per_meter) * (step_m * units_per_meter),
                                   sigma_s_m * step_m)

    def test_skyrim_disk_bound_fits_normalized_material_storage(self):
        units_per_meter = 69.99125
        radius_m = 16.0 / units_per_meter
        self.assertGreater(radius_m, 0)
        self.assertLess(radius_m, 1)
        decoded_m = round(radius_m * 255) / 255
        self.assertLess(abs(decoded_m * units_per_meter - 16.0), units_per_meter / 510)

    def test_runtime_converts_shared_diffusion_and_transmission_material(self):
        source = (ROOT / '.research/dxvk-remix/src/dxvk/shaders/rtx/algorithm/rtxcr/rtxcr_material.slangh').read_text()
        self.assertIn('cb.sssArgs.diffusionProfileScale * cb.metersToWorldUnitScale;', source)
        self.assertIn('const float thickness = transmissionTracingResult.hitDistance;', source)
        self.assertIn('RTXCR_ComputeSubsurfaceMaterialCoefficients(subsurfaceMaterialData)', source)

    def test_host_uses_engine_scale_and_meter_disk_bound(self):
        bridge = (ROOT / 'src/RemixBridge.cpp').read_text()
        scene = (ROOT / 'src/RemixScene.cpp').read_text()
        self.assertIn('metersToWorldUnits = RE::bhkWorld::GetWorldScaleInverse()', bridge)
        self.assertIn('metersToWorldUnits * 0.01f', bridge)
        self.assertIn('api.SetConfigVariable("rtx.sceneScale", sceneScale.c_str())', bridge)
        self.assertIn('subsurfaceMaxSampleRadius = 16.0f * RE::bhkWorld::GetWorldScale()', scene)


if __name__ == '__main__':
    unittest.main()
