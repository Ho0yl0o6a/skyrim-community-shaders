param(
    [Parameter(Mandatory = $true)][ValidateRange(1, 10000)][double]$WorldUnitsPerMeter,
    [string]$Label = 'skin-units',
    [double[]]$Position,
    [double]$Pitch = 0,
    [double]$Yaw = 0
)
$ErrorActionPreference = 'Stop'
function Invoke-SkinTool([string]$Name, [hashtable]$Body) {
    Invoke-RestMethod "http://127.0.0.1:8920/api/tool/$Name" -Method Post -ContentType application/json -Body ($Body | ConvertTo-Json -Compress) -TimeoutSec 10
}
function Set-SkinScale([double]$Value) {
    $encoded = $Value.ToString('R', [Globalization.CultureInfo]::InvariantCulture)
    if (!(Invoke-SkinTool 'communityshaders.remix' @{name='rtx.subsurface.diffusionProfileScale';value=$encoded}).success) {
        throw 'Rejected diffusion scale'
    }
}
# Fresh configured run only: known default scale=1, debug=0. The legacy control
# cancels coefficient conversion, but retains the corrected disk search bound.
# It is not a complete reproduction of the old renderer.
if ((Invoke-SkinTool 'camera' @{action='get'}).freeCam) { throw 'Start outside free camera.' }
$frozen = $false
try {
    $null = Invoke-SkinTool 'console' @{command='tfc 1'}
    Start-Sleep -Milliseconds 700
    $frozen = (Invoke-SkinTool 'camera' @{action='get'}).freeCam
    if (!$frozen) { throw 'Freeze camera did not engage.' }
    if ($Position) {
        if ($Position.Count -ne 3) { throw 'Position needs exactly three coordinates.' }
        $null = Invoke-SkinTool 'camera' @{action='drive';x=$Position[0];y=$Position[1];z=$Position[2];pitch=$Pitch;yaw=$Yaw}
    }
    Set-SkinScale 1
    & "$PSScriptRoot/Capture.ps1" -NoFreecam -Label "$Label-corrected" -SettleMs 4000
    Set-SkinScale (1 / $WorldUnitsPerMeter)
    & "$PSScriptRoot/Capture.ps1" -NoFreecam -Label "$Label-legacy-coefficients" -SettleMs 4000
    Set-SkinScale 1
    & "$PSScriptRoot/Capture.ps1" -NoFreecam -Label "$Label-corrected-repeat" -SettleMs 4000
} finally {
    try { Set-SkinScale 1 }
    finally {
        if ($frozen -and (Invoke-SkinTool 'camera' @{action='get'}).freeCam) {
            $null = Invoke-SkinTool 'console' @{command='tfc 1'}
        }
    }
}
