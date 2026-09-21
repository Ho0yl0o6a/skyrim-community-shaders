"""CPU/source guards for the diagnostic, not visual or GPU correctness tests."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2] / '.research/dxvk-remix/src/dxvk'


class PairedSurfaceView(unittest.TestCase):
    def test_unique_index(self):
        text = (ROOT / 'shaders/rtx/utility/debug_view_indices.h').read_text()
        definitions = re.findall(r'#define (DEBUG_VIEW_\w+) (\d+)\b', text)
        self.assertEqual([name for name, value in definitions if value == '823'],
                         ['DEBUG_VIEW_PAIRED_SURFACE_FRAME'])

    def test_same_source_coordinates_in_each_panel(self):
        for width, height in ((1920, 1080), (1280, 720)):
            for x, y in ((0, 0), (295, 145), (width // 2 - 1, height // 2 - 1)):
                coordinates = [((2 * (x + col * width // 2)) % width,
                                (2 * (y + row * height // 2)) % height)
                               for row in range(2) for col in range(2)]
                self.assertEqual(len(set(coordinates)), 1)
                self.assertEqual(coordinates[0], (2 * x, 2 * y))

    def test_paired_composite_uses_current_hdr_input(self):
        indices = (ROOT / 'shaders/rtx/utility/debug_view_indices.h').read_text()
        self.assertEqual(re.findall(r'#define (DEBUG_VIEW_\w+) 824\b', indices),
                         ['DEBUG_VIEW_PAIRED_COMPOSITE_FRAME'])
        shader = (ROOT / 'shaders/rtx/pass/debug_view/debug_view.comp.slang').read_text()
        self.assertIn('max(Composite[sourcePixel].xyz, vec3(0.0f))', shader)
        self.assertIn('hdr / (vec3(1.0f) + hdr)', shader)
        dispatch = (ROOT / 'rtx_render/rtx_debug_view.cpp').read_text()
        begin = dispatch.index('bool DebugView::shouldRunDispatchPostCompositePass()')
        self.assertIn('DEBUG_VIEW_PAIRED_COMPOSITE_FRAME', dispatch[begin:begin + 300])
        context = (ROOT / 'rtx_render/rtx_context.cpp').read_text()
        self.assertIn('DEBUG_VIEW_PAIRED_COMPOSITE_FRAME', context)

    def test_albedo_written_and_diagnostic_history_bypassed(self):
        resolver = (ROOT / 'shaders/rtx/algorithm/geometry_resolver.slangh').read_text()
        self.assertEqual(resolver.count('case DEBUG_VIEW_PAIRED_SURFACE_FRAME:\n  case DEBUG_VIEW_ALBEDO:'), 2)
        shader = (ROOT / 'shaders/rtx/pass/debug_view/debug_view.comp.slang').read_text()
        begin = shader.index('vec4 loadInput(')
        self.assertGreater(shader.index('case DEBUG_VIEW_PAIRED_SURFACE_FRAME:'), begin)
        self.assertIn('AccumulatedDebugView[threadId] = value;', shader)
        dispatch = (ROOT / 'rtx_render/rtx_debug_view.cpp').read_text()
        begin = dispatch.index('bool DebugView::shouldRunDispatchPostCompositePass()')
        self.assertIn('DEBUG_VIEW_PAIRED_SURFACE_FRAME', dispatch[begin:begin + 240])
        self.assertIn('return false;', dispatch[begin:begin + 650])

    def test_paired_material_inputs(self):
        indices = (ROOT / 'shaders/rtx/utility/debug_view_indices.h').read_text()
        self.assertEqual(re.findall(r'#define (DEBUG_VIEW_\w+) 825\b', indices),
                         ['DEBUG_VIEW_PAIRED_MATERIAL_FRAME'])
        demodulate = (ROOT / 'shaders/rtx/pass/demodulate/demodulate.comp.slang').read_text()
        self.assertIn('case DEBUG_VIEW_PAIRED_MATERIAL_FRAME:\n  case DEBUG_VIEW_PRIMARY_SPECULAR_ALBEDO:', demodulate)
        shader = (ROOT / 'shaders/rtx/pass/debug_view/debug_view.comp.slang').read_text()
        self.assertIn('unormVectorToColor(PrimaryVirtualWorldNormalPerceptualRoughness[sourcePixel].xyz)', shader)
        self.assertIn('vec3(PrimaryVirtualWorldNormalPerceptualRoughness[sourcePixel].w)', shader)
        self.assertRegex(shader, r'cb.debugViewIdx == DEBUG_VIEW_PAIRED_MATERIAL_FRAME[^{}]+\{\s*AccumulatedDebugView\[threadId\] = value;')
        dispatch = (ROOT / 'rtx_render/rtx_debug_view.cpp').read_text()
        begin = dispatch.index('bool DebugView::shouldRunDispatchPostCompositePass()')
        self.assertIn('DEBUG_VIEW_PAIRED_MATERIAL_FRAME', dispatch[begin:begin + 400])
        self.assertIn('return false;', dispatch[begin:begin + 650])
        self.assertIn('DEBUG_VIEW_PAIRED_MATERIAL_FRAME', (ROOT / 'rtx_render/rtx_context.cpp').read_text())

    def test_paired_alpha_composite_terms(self):
        indices = (ROOT / 'shaders/rtx/utility/debug_view_indices.h').read_text()
        self.assertEqual(re.findall(r'#define (DEBUG_VIEW_\w+) 826\b', indices),
                         ['DEBUG_VIEW_PAIRED_ALPHA_COMPOSITE_FRAME'])
        composite = (ROOT / 'shaders/rtx/pass/composite/composite.comp.slang').read_text()
        self.assertIn('calcBt709Luminance(max(radianceOutput * backgroundAlpha, vec3(0.0f)))', composite)
        self.assertIn('calcBt709Luminance(max(alphaBlendOutput, vec3(0.0f))), backgroundAlpha)', composite)
        shader = (ROOT / 'shaders/rtx/pass/debug_view/debug_view.comp.slang').read_text()
        self.assertIn('const vec3 terms = DebugView[sourcePixel].xyz;', shader)
        self.assertIn('all(panel) && cb.debugViewIdx == DEBUG_VIEW_PAIRED_ALPHA_COMPOSITE_FRAME', shader)
        self.assertRegex(shader, r'cb.debugViewIdx == DEBUG_VIEW_PAIRED_ALPHA_COMPOSITE_FRAME[^{}]+\{\s*AccumulatedDebugView\[threadId\] = value;')

    def test_paired_lighting_terms(self):
        indices = (ROOT / 'shaders/rtx/utility/debug_view_indices.h').read_text()
        self.assertEqual(re.findall(r'#define (DEBUG_VIEW_\w+) 827\b', indices),
                         ['DEBUG_VIEW_PAIRED_LIGHTING_FRAME'])
        composite = (ROOT / 'shaders/rtx/pass/composite/composite.comp.slang').read_text()
        self.assertIn('primaryCombinedDiffuseRadiance * primaryAlbedo * primaryAttenuation', composite)
        self.assertIn('primaryCombinedSpecularRadiance * primarySpecularAlbedo * primaryAttenuation / roughnessFactor', composite)
        self.assertIn('max(remodulatedTotalSecondaryRadiance + sharedRadiance, vec3(0.0f))', composite)
        body = composite[composite.index('vec4 compositeResult('):]
        self.assertLess(body.index('storeInDebugView(pixelCoordinate, vec3(0.0f))'),
                        body.index('loadInputRadiance('))
        shader = (ROOT / 'shaders/rtx/pass/debug_view/debug_view.comp.slang').read_text()
        self.assertIn('case DEBUG_VIEW_PAIRED_LIGHTING_FRAME:', shader)
        self.assertIn('DEBUG_VIEW_PAIRED_LIGHTING_FRAME)\n    {\n      AccumulatedDebugView[threadId] = value;', shader)
        for name in ('rtx_context.cpp', 'rtx_debug_view.cpp'):
            self.assertIn('DEBUG_VIEW_PAIRED_LIGHTING_FRAME', (ROOT / 'rtx_render' / name).read_text())


if __name__ == '__main__':
    unittest.main()
