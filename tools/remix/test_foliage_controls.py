"""Source-contract checks for live SSS controls on retained materials.

GPU debug-view tests are still required to establish runtime behavior.
"""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2] / ".research/dxvk-remix/src/dxvk"


class FoliageControlTests(unittest.TestCase):
    def test_retained_thin_material_checks_live_switch(self):
        text = (ROOT / "shaders/rtx/concept/surface_material/surface_material_helper.slangh").read_text()
        body = text.split("bool isThinOpaqueSubsurfaceMaterial(", 1)[1].split("\n}", 1)[0]
        self.assertIn("cb.sssArgs.enableThinOpaque", body)
        self.assertIn("measurementDistance > 0.0h", body)
        self.assertIn("maxSampleRadius == 0", body)

    def test_native_scattering_respects_texture_switch(self):
        text = (ROOT / "shaders/rtx/concept/surface_material/opaque_surface_material_interaction.slangh").read_text()
        self.assertRegex(text, re.compile(
            r"if \(\(opaqueSurfaceMaterial.flags & OPAQUE_SURFACE_MATERIAL_FLAG_NATIVE_FOLIAGE\) &&\s*"
            r"cb.sssArgs.enableThinOpaque && cb.sssArgs.enableTextureMaps\)"))

    def test_generic_sss_maps_respect_live_switch(self):
        text = (ROOT / "shaders/rtx/concept/surface_material/opaque_surface_material_interaction.slangh").read_text()
        body = text.split("SubsurfaceMaterialInteraction subsurfaceMaterialInteractionCreate(", 1)[1].split("\n}", 1)[0]
        for name in ("measurementDistanceLoaded", "transmittanceColorLoaded", "singleScatteringAlbedoLoaded"):
            self.assertRegex(body, rf"bool {name} = cb\.sssArgs\.enableTextureMaps &&")

    def test_native_scattering_preserves_cs_colour_products(self):
        text = (ROOT / "shaders/rtx/concept/surface_material/opaque_surface_material_interaction.slangh").read_text()
        body = text.split("NativeFoliageResponse nativeFoliageResponseCreate(", 1)[1].split("\n}", 1)[0]
        grass, tree = body.split("vec4 soft =", 1)
        self.assertIn("nativeFoliageLinearColor(foliage, base * base)", grass)
        for channel in "rgb":
            self.assertIn(f"nativeFoliageLightingChannel(base.{channel}, soft.{channel}, gammaSpace)", tree)
            self.assertIn(f"nativeFoliageLightingChannel(base.{channel}, back.{channel}, gammaSpace)", tree)
        math = (ROOT / "shaders/rtx/concept/surface_material/native_foliage_math.h").read_text()
        self.assertIn("nativeFoliageLinearChannel(baseColor, gammaSpace) * lightingWeight", math)


if __name__ == "__main__":
    unittest.main()
