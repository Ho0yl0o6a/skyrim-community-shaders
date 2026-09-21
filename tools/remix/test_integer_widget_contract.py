"""Guard integral option wrappers against bypassing their typed adapters."""

from pathlib import Path
import unittest


SOURCE = Path(__file__).resolve().parents[2] / ".research/dxvk-remix/src/dxvk/rtx_render/rtx_imgui.h"


class IntegerWidgetContractTests(unittest.TestCase):
    def test_option_wrappers_preserve_value_pointer_type(self):
        source = SOURCE.read_text()
        for widget in ("Combo", "DragInt", "InputInt", "SliderInt"):
            with self.subTest(widget=widget):
                signature = f"bool {widget}(const char* label, dxvk::RtxOption<T>* rtxOption"
                body = source.split(signature, 1)[1].split("\n  }", 1)[0]
                self.assertIn("label, &value,", body)
                self.assertNotIn("(int*)", body)


if __name__ == "__main__":
    unittest.main()
