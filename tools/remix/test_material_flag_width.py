"""Ensure shared opaque material flags fit the actual packed GPU field."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2] / '.research/dxvk-remix/src/dxvk'


class MaterialFlagWidthTests(unittest.TestCase):
    def test_opaque_flags_fit_and_do_not_overlap(self):
        constants = (ROOT / 'shaders/rtx/utility/shared_constants.h').read_text()
        shift = int(re.search(r'#define COMMON_MATERIAL_FLAG_TYPE_OFFSET\(X\) \((\d+) \+ X\)', constants)[1])
        flags = re.findall(r'#define (OPAQUE_SURFACE_MATERIAL_FLAG_\w+) \(1 << COMMON_MATERIAL_FLAG_TYPE_OFFSET\((\d+)\)\)', constants)
        self.assertGreater(len(flags), 8)
        used = (1 << shift) - 1
        for name, offset in flags:
            value = 1 << (shift + int(offset))
            with self.subTest(flag=name):
                self.assertLessEqual(value, 0xffff)
                self.assertEqual(value & used, 0)
            used |= value
        layout = (ROOT / 'shaders/rtx/concept/surface_material/surface_material.h').read_text()
        body = layout.split('struct OpaqueSurfaceMaterial', 1)[1].split('};', 1)[0]
        self.assertIn('uint16_t flags;', body)


if __name__ == '__main__':
    unittest.main()
