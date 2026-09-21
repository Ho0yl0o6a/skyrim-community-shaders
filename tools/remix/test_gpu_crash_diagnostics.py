import unittest
from pathlib import Path


class GpuCrashDiagnosticsTests(unittest.TestCase):
    def test_launch_diagnostics_are_opt_in_and_normal_launch_clears_them(self):
        source = Path(__file__).with_name('LaunchTest.ps1').read_text()
        self.assertIn('[switch]$GpuCrashDiagnostics', source)
        for variable in ('DXVK_ENABLE_AFTERMATH', 'DXVK_ENABLE_AFTERMATH_RESOURCE_TRACKING'):
            self.assertIn(f"$env:{variable} = '1'", source)
            self.assertIn(f'Remove-Item Env:{variable}', source)
        self.assertLess(source.index('if ($GpuCrashDiagnostics)'), source.index('Start-Process'))

    def test_decoder_converts_shader_info_to_comparable_binary_hash(self):
        source = Path(__file__).with_name('DecodeGpuCrash.py').read_text()
        self.assertIn('GFSDK_Aftermath_GetShaderHashForShaderInfo', source)
        self.assertIn("'Comparable shader binary hashes'", source)
        self.assertIn("checked('DestroyDecoder', decoder)", source)
