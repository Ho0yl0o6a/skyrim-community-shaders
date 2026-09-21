param(
    [string]$Label = 'skin-transmission',
    [double[]]$Position,
    [double]$Pitch = 0,
    [double]$Yaw = 0
)
$ErrorActionPreference = 'Stop'
function Invoke-SkinTool([string]$Name, [hashtable]$Body) {
    Invoke-RestMethod "http://127.0.0.1:8920/api/tool/$Name" -Method Post -ContentType application/json -Body ($Body | ConvertTo-Json -Compress) -TimeoutSec 10
}
function Set-SkinOption([string]$Name, [string]$Value) {
    if (!(Invoke-SkinTool 'communityshaders.remix' @{name=$Name;value=$Value}).success) { throw "Rejected $Name" }
}
function Capture-SkinComparison([string]$Suffix) {
    $capture = & "$PSScriptRoot/Capture.ps1" -NoFreecam -Label "$Label-$Suffix" -SettleMs 2500
    $capture
    $observed = ($capture | ConvertFrom-Json).camera
    foreach ($field in @('camX','camY','camZ','camPitch','camYaw')) {
        if ([Math]::Abs([double]$observed.$field - [double]$script:comparisonCamera.$field) -gt 0.001) {
            throw "Camera moved during comparison ($field); reject captures."
        }
    }
}
# Use only in a freshly configured test run with these documented defaults.
if ((Invoke-SkinTool 'camera' @{action='get'}).freeCam) { throw 'Start outside free camera for a known freeze state.' }
$frozen = $false
try {
    $null = Invoke-SkinTool 'console' @{command='tfc 1'}
    Start-Sleep -Milliseconds 700
    $frozen = (Invoke-SkinTool 'camera' @{action='get'}).freeCam
    if (!$frozen) { throw 'Freeze camera did not engage.' }
    if ($Position) {
        if ($Position.Count -ne 3) { throw 'Position needs exactly three coordinates.' }
        $null = Invoke-SkinTool 'camera' @{action='drive';x=$Position[0];y=$Position[1];z=$Position[2];pitch=$Pitch;yaw=$Yaw}
        Start-Sleep -Milliseconds 700
    }
    $script:comparisonCamera = Invoke-SkinTool 'camera' @{action='get'}
    Set-SkinOption 'rtx.debugView.debugViewIdx' '0'
    Capture-SkinComparison 'default'
    Set-SkinOption 'rtx.subsurface.enableTransmission' 'false'
    Capture-SkinComparison 'no-transmission'
    Set-SkinOption 'rtx.subsurface.diffusionProfileScale' '0.1'
    Capture-SkinComparison 'small-diffusion'
    Set-SkinOption 'rtx.subsurface.enableDiffusionProfile' 'false'
    Capture-SkinComparison 'no-diffusion-or-transmission'
} finally {
    try {
        try { Set-SkinOption 'rtx.subsurface.enableDiffusionProfile' 'true' }
        finally { Set-SkinOption 'rtx.subsurface.diffusionProfileScale' '1.0' }
    }
    finally {
        try { Set-SkinOption 'rtx.subsurface.enableTransmission' 'true' }
        finally {
            if ($frozen -and (Invoke-SkinTool 'camera' @{action='get'}).freeCam) {
                $null = Invoke-SkinTool 'console' @{command='tfc 1'}
                Start-Sleep -Milliseconds 700
            }
        }
    }
}
(Invoke-SkinTool 'camera' @{action='get'}) | ConvertTo-Json -Compress
