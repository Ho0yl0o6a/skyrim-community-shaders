"""Debug-view coverage contracts; these do not establish rendered correctness."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2] / ".research/dxvk-remix/src/dxvk"


class DirectLightDiagnosticsTests(unittest.TestCase):
    def test_invalid_light_samples_are_marked_before_nee_guard(self):
        source = (ROOT / "shaders/rtx/algorithm/integrator_direct.slangh").read_text()
        body = source.split("void integrateDirectPath(", 1)[1]
        before_guard = body.split("if (rtxdiLightSampleValid || risLightSampleValid || isDiffusionProfileSss)", 1)[0]
        self.assertIn("!rtxdiLightSampleValid && !risLightSampleValid", before_guard)
        self.assertIn("cb.debugView == DEBUG_VIEW_DIRECT_VISIBILITY_STATE", before_guard)
        self.assertIn("storeInDebugView(pixelCoordinate, vec3(1, 0, 0))", before_guard)

    def test_facing_view_uses_actual_material_normal_and_view_direction(self):
        source = (ROOT / "shaders/rtx/algorithm/geometry_resolver.slangh").read_text()
        body = source.split("case DEBUG_VIEW_SHADING_NORMAL_FACING:", 1)[1].split("case DEBUG_VIEW_VERTEX_COLOR:", 1)[0]
        self.assertIn("opaqueSurfaceMaterialInteractionCreate(polymorphicSurfaceMaterialInteraction).shadingNormal", body)
        self.assertIn("dot(normal, rayInteraction.viewDirection)", body)
        self.assertIn("isnan(normalDotView) || isinf(normalDotView)", body)
        self.assertIn("normalDotView < materialEpsilon", body)


if __name__ == "__main__":
    unittest.main()
