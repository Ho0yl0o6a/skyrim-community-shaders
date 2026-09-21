import unittest
from pathlib import Path


class PassiveCaptureTests(unittest.TestCase):
    def test_audit_waits_for_bounded_frame_progress(self):
        source = Path(__file__).with_name('CapturePlayerMovement.ps1').read_text()
        self.assertIn('$request.enqueued_at_frame + 65', source)
        self.assertIn('[DateTime]::UtcNow.AddSeconds(8)', source)
        self.assertNotIn('Start-Sleep -Milliseconds 2500', source)
        self.assertIn("} finally {", source)

    def test_weapon_transition_is_opt_in_and_after_capture_queue(self):
        source = Path(__file__).with_name('CapturePlayerMovement.ps1').read_text()
        self.assertIn("if ($ObserveOnly -and $WeaponAction -ne 'None')", source)
        self.assertLess(source.index("if (!$request.queued)"), source.index("if ($WeaponAction -ne 'None')"))
        self.assertIn('$manifest.weaponBefore = $weaponBefore', source)
        self.assertIn('$manifest.weaponAfter = $weaponAfter', source)
        self.assertIn('$weaponBefore.returned -eq $wantDrawn -or $weaponAfter.returned -ne $wantDrawn', source)

    def test_observer_never_sets_pov_or_injects_input(self):
        source = Path(__file__).with_name('CapturePlayerMovement.ps1').read_text()
        self.assertIn('[switch]$ObserveOnly', source)
        self.assertIn("if (!$ObserveOnly) { $null = DB camera @{action='setPov';pov=$Pov} }", source)
        for operation in ('HoldKey', 'ReleaseKey'):
            line = next(line for line in source.splitlines() if f"function='{operation}'" in line)
            self.assertIn('if (!$ObserveOnly -and !$Stationary)', line)
        self.assertIn('observeOnly=[bool]$ObserveOnly', source)
        self.assertIn('cameraModeMismatch=$modeMismatch', source)
