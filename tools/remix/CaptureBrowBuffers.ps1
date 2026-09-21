param([string]$Label = 'brow-buffers', [switch]$IsolateAlphaTest, [switch]$IsolateOmmBinding,
    [switch]$HitDiagnostics)
$ErrorActionPreference = 'Stop'
function Invoke-BrowTool([string]$Name, [hashtable]$Body) {
    Invoke-RestMethod "http://127.0.0.1:8920/api/tool/$Name" -Method Post -ContentType application/json -Body ($Body | ConvertTo-Json -Compress) -TimeoutSec 10
}
function Check-BrowCamera {
    $observed = Invoke-BrowTool camera @{action='get'}
    foreach ($field in @('camX','camY','camZ','camPitch','camYaw')) {
        if ([Math]::Abs([double]$observed.$field - [double]$script:browReference.$field) -gt .001) {
            throw "Camera moved ($field); reject comparison."
        }
    }
}
# Pair mode requires LaunchTest -MatchCaptureSamples. Fixed camera belongs to
# Riverwood Save1; verify the screenshots still contain the intended character.
if ((Invoke-BrowTool camera @{action='get'}).freeCam) { throw 'Start outside free camera.' }
$frozen = $false
$isolationOption = if ($IsolateOmmBinding) { 'rtx.opacityMicromap.enableBinding' } else { 'rtx.enableAlphaTest' }
if ($IsolateOmmBinding -and $IsolateAlphaTest) { throw 'Change only one option per isolation.' }
try {
    $null = Invoke-BrowTool camera @{action='setPov';pov='third'}
    Start-Sleep -Milliseconds 700
    $null = Invoke-BrowTool console @{command='tfc 1'}
    Start-Sleep -Milliseconds 700
    $frozen = (Invoke-BrowTool camera @{action='get'}).freeCam
    if (!$frozen) { throw 'Freeze did not engage.' }
    $null = Invoke-BrowTool camera @{action='drive';x=13735;y=-48155;z=-158;pitch=0;yaw=-2.07738137}
    Start-Sleep -Milliseconds 700
    $script:browReference = Invoke-BrowTool camera @{action='get'}
    & "$PSScriptRoot/Capture.ps1" -NoFreecam -Label "$Label-before" -SettleMs 3000
    Check-BrowCamera
    & "$PSScriptRoot/CaptureBuffers.ps1" -Mode Pair -DebugView 0 -Label "$Label-lit"
    Check-BrowCamera
    & "$PSScriptRoot/Capture.ps1" -NoFreecam -Label "$Label-after-lit" -SettleMs 2500
    & "$PSScriptRoot/CaptureBuffers.ps1" -Mode Pair -DebugView 808 -Label "$Label-facing"
    Check-BrowCamera
    & "$PSScriptRoot/Capture.ps1" -NoFreecam -Label "$Label-after-facing" -SettleMs 2500
    Check-BrowCamera
    if ($HitDiagnostics) {
        foreach ($mode in @('Pair','Remix')) {
            foreach ($view in @(817,818)) {
                & "$PSScriptRoot/CaptureBuffers.ps1" -Mode $mode -DebugView $view -Label "$Label-$mode-$view"
                Check-BrowCamera
            }
        }
        if (!(Invoke-BrowTool communityshaders.remix @{name='rtx.debugView.showFirstGBufferHit';value='true'}).success) {
            throw 'First-hit diagnostic rejected.'
        }
        Start-Sleep -Milliseconds 1000
        foreach ($view in @(817,818,819)) {
            & "$PSScriptRoot/CaptureBuffers.ps1" -Mode Pair -DebugView $view -Label "$Label-first-hit-$view"
            Check-BrowCamera
        }
        $null = Invoke-BrowTool communityshaders.remix @{name='rtx.debugView.showFirstGBufferHit';value='false'}
    }
    if ($IsolateAlphaTest -or $IsolateOmmBinding) {
        # Both options default to true in a fresh test run. Verify the intended
        # geometry/acceleration change actually occurred before interpreting it.
        if (!(Invoke-BrowTool communityshaders.remix @{name=$isolationOption;value='false'}).success) {
            throw 'Visibility isolation option rejected.'
        }
        Start-Sleep -Milliseconds 2500
        & "$PSScriptRoot/CaptureBuffers.ps1" -Mode Pair -DebugView 0 -Label "$Label-option-off"
        & "$PSScriptRoot/Capture.ps1" -NoFreecam -Label "$Label-option-off" -SettleMs 2500
        Check-BrowCamera
        $null = Invoke-BrowTool communityshaders.remix @{name=$isolationOption;value='true'}
        Start-Sleep -Milliseconds 2500
        & "$PSScriptRoot/CaptureBuffers.ps1" -Mode Pair -DebugView 0 -Label "$Label-restored"
        & "$PSScriptRoot/Capture.ps1" -NoFreecam -Label "$Label-restored" -SettleMs 2500
        Check-BrowCamera
    }
} finally {
    try {
        if ($HitDiagnostics) {
            $null = Invoke-BrowTool communityshaders.remix @{name='rtx.debugView.showFirstGBufferHit';value='false'}
        }
        if ($IsolateAlphaTest -or $IsolateOmmBinding) {
            $null = Invoke-BrowTool communityshaders.remix @{name=$isolationOption;value='true'}
        }
    } finally {
        try { $null = Invoke-BrowTool communityshaders.remix @{name='rtx.debugView.debugViewIdx';value='0'} }
        finally {
            if ($frozen -and (Invoke-BrowTool camera @{action='get'}).freeCam) {
                $null = Invoke-BrowTool console @{command='tfc 1'}
            }
        }
    }
}
