"""Structural traversal contracts; not proof of GPU visibility or canopy appearance."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2] / ".research/dxvk-remix/src/dxvk"


class ThinTransmissionFlagsTests(unittest.TestCase):
    def test_thin_materials_request_one_nonopaque_candidate_per_primitive(self):
        source = (ROOT / "rtx_render/rtx_instance_manager.cpp").read_text()
        body = source.split("void InstanceManager::updateInstance(", 1)[1]
        classification = body.split("const bool hasThinTransmission =", 1)[1].split(";", 1)[0]
        self.assertIn("materialData->getType() == MaterialDataType::Opaque", classification)
        self.assertIn("!materialData->getOpaqueMaterialData().getSubsurfaceDiffusionProfile()", classification)
        self.assertIn("getSubsurfaceMeasurementDistance() > 0.0f", classification)
        flags = body.split("if (hasThinTransmission) {", 1)[1].split("\n    }", 1)[0]
        self.assertIn("m_vkInstance.flags |= VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR", flags)
        self.assertIn("m_geometryFlags |= VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR", flags)
        self.assertLess(body.index("if (hasThinTransmission)"), body.index("const bool accelerationStructureKeyChanged"))
        self.assertGreater(body.index("if (hasThinTransmission)"), body.index("// Update the geometry and instance flags"))

    def test_transmission_does_not_change_material_optics(self):
        source = (ROOT / "rtx_render/rtx_instance_manager.cpp").read_text()
        flags = source.split("if (hasThinTransmission) {", 1)[1].split("\n    }", 1)[0]
        self.assertNotIn("setSubsurface", flags)
        self.assertNotIn("RtxOptions", flags)
        self.assertNotIn("m_isSubsurface =", flags)

    def test_native_visibility_uses_native_response_without_a_phase_penalty(self):
        source = (ROOT / "shaders/rtx/algorithm/visibility.slangh").read_text()
        body = source.split("if (usesNativeFoliageResponse(opaqueSurfaceMaterialInteraction))", 1)[1].split("\n      }", 1)[0]
        self.assertIn("-abs(dot(ray.direction, surfaceInteraction.triangleNormal))", body)
        self.assertIn("nativeFoliageProjectedWeight(opaqueSurfaceMaterialInteraction, angle) * pi", body)
        self.assertNotIn("shadingNormal", body)
        self.assertNotIn("fourPi", body)

    def test_transmission_keeps_the_native_soft_and_back_weights(self):
        source = (ROOT / "shaders/rtx/concept/surface_material/opaque_surface_material_interaction.slangh").read_text()
        body = source.split("NativeFoliageResponse nativeFoliageResponseCreate(", 1)[1].split("// NV-DXVK end", 1)[0]
        self.assertIn("nativeFoliageLinearColor(foliage, base * base)", body)
        self.assertIn("interaction.vertexColor.a * 10.0f", body)
        self.assertIn("foliage.scatteringAmount * 2.0f", body)
        self.assertIn("NATIVE_FOLIAGE_SOFT", body)
        self.assertIn("NATIVE_FOLIAGE_BACK", body)
        for channel in "rgb":
            self.assertIn(f"nativeFoliageLightingChannel(base.{channel}, soft.{channel}, gammaSpace)", body)
            self.assertIn(f"nativeFoliageLightingChannel(base.{channel}, back.{channel}, gammaSpace)", body)
        self.assertEqual(body.count("nativeFoliageResponseChannel("), 3)
        math = (ROOT / "shaders/rtx/concept/surface_material/native_foliage_math.h").read_text()
        self.assertIn("return softColor * nativeSoftLightMultiplier(angle, rolloff) + backColor * nativeFoliageSaturate(-angle);", math)

    def test_native_response_payload_survives_existing_gbuffer_flag_mask(self):
        constants = (ROOT / "shaders/rtx/utility/shared_constants.h").read_text()
        self.assertIn("OPAQUE_SURFACE_MATERIAL_INTERACTION_FLAG_NATIVE_FOLIAGE (1 << 2)", constants)
        self.assertIn("OPAQUE_SURFACE_MATERIAL_INTERACTION_FLAG_MASK 0x3F", constants)
        source = (ROOT / "shaders/rtx/concept/surface_material/opaque_surface_material_interaction.slangh").read_text()
        self.assertIn("measurementDistance = float16_t(response.rolloff + 1.0f)", source)
        self.assertIn("float(data.measurementDistance) - 1.0f", source)
        self.assertIn("packedTransmittanceColor = colorToR11G11B10(max(response.softColor", source)
        self.assertIn("packedSingleScatteringAlbedo = colorToR11G11B10(max(response.backColor", source)
        self.assertEqual(source.count("diffuseReflectionWeight * normalDotInputDirection + nativeWeight"), 2)
        # Both direct evaluation and sample throughput consume projected weight directly.
        self.assertIn("nativeSample.throughput = saferPositiveDivide(nativeFoliageProjectedWeight", source)
        self.assertRegex(source, re.compile(r"diffuseTransmissionWeight = nativeFoliage \? nativeWeight :"))

    def test_rtxdi_reduced_reader_identifies_native_payload(self):
        source = (ROOT / "shaders/rtx/algorithm/rtxdi/RtxdiApplicationBridge.slangh").read_text()
        body = source.split("RAB_Surface RAB_GetGBufferSurface(", 1)[1].split("#ifdef RAB_HAS_CURRENT_GBUFFER", 1)[1].split("surface.virtualWorldPosition", 1)[0]
        self.assertIn("sourceMaterial = opaqueSurfaceMaterialCreate(surfaceMaterials[primarySurfaceIndex])", body)
        self.assertIn("sourceMaterial.flags & OPAQUE_SURFACE_MATERIAL_FLAG_NATIVE_FOLIAGE", body)
        self.assertIn("cb.sssArgs.enableThinOpaque && cb.sssArgs.enableTextureMaps", body)
        self.assertLess(body.index("primarySurfaceIndex != SURFACE_INDEX_INVALID"), body.index("sourceMaterial ="))
        self.assertLess(body.index("isThinOpaqueSubsurfaceMaterial(surface.opaqueSurfaceMaterialInteraction)"), body.index("sourceMaterial ="))
        self.assertIn("surface.opaqueSurfaceMaterialInteraction.flags |= OPAQUE_SURFACE_MATERIAL_INTERACTION_FLAG_NATIVE_FOLIAGE", body)

    def test_only_visibility_omits_tree_diffuse_colour_from_transmission(self):
        source = (ROOT / "shaders/rtx/concept/surface_material/opaque_surface_material_interaction.slangh").read_text()
        body = source.split("NativeFoliageResponse nativeFoliageResponseCreate(", 1)[1].split("\nbool usesNativeFoliageResponse", 1)[0]
        self.assertIn("bool transmissionOnly = false", body)
        self.assertIn("if (transmissionOnly)", body)
        self.assertIn("base = vec3(1.0f)", body)
        self.assertLess(body.index("return response;"), body.index("if (transmissionOnly)"))
        self.assertIn("bool nativeTransmissionOnly = false", source)
        self.assertIn("complexGrass, nativeTransmissionOnly)", source)
        visibility = (ROOT / "shaders/rtx/algorithm/visibility.slangh").read_text()
        self.assertIn("rayInteraction, cb.debugKnob.w != 1.0f)", visibility)
        context = (ROOT / "rtx_render/rtx_context.cpp").read_text()
        self.assertIn("constants.debugKnob.w = debugView.foliageDiffuseTransmission() ? 1.0f : 0.0f", context)


if __name__ == "__main__":
    unittest.main()
