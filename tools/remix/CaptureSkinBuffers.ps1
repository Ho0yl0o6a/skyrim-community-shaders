param(
    [string]$Label = 'skin-buffers',
    [ValidateSet('Remix', 'Pair')][string]$Mode = 'Remix',
    [switch]$AuthoredNormals,
    [ValidateCount(3,3)][double[]]$Position = @(13655,-48295,-170.8372),
    [double]$Pitch = 0.157043,
    [double]$Yaw = -0.0800914
)
$ErrorActionPreference = 'Stop'
function Invoke-SkinTool([string]$Name, [hashtable]$Body) {
    Invoke-RestMethod "http://127.0.0.1:8920/api/tool/$Name" -Method Post -ContentType application/json -Body ($Body | ConvertTo-Json -Compress) -TimeoutSec 10
}
if ((Invoke-SkinTool 'camera' @{action='get'}).freeCam) { throw 'Start outside free camera for a known freeze state.' }
$frozen = $false
try {
    $null = Invoke-SkinTool 'camera' @{action='setPov';pov='third'}
    Start-Sleep -Milliseconds 700
    $null = Invoke-SkinTool 'console' @{command='tfc 1'}
    Start-Sleep -Milliseconds 700
    $frozen = (Invoke-SkinTool 'camera' @{action='get'}).freeCam
    if (!$frozen) { throw 'Freeze camera did not engage.' }
    $null = Invoke-SkinTool 'camera' @{action='drive';x=$Position[0];y=$Position[1];z=$Position[2];pitch=$Pitch;yaw=$Yaw}
    Start-Sleep -Seconds 2
    # Classification and light validity are separate frames. Jitter can differ;
    # they cannot establish exact per-pixel correspondence or renderer parity.
    if ($AuthoredNormals) {
        & "$PSScriptRoot/CaptureBuffers.ps1" -Mode $Mode -DebugView 815 -Label "$Label-authored"
        & "$PSScriptRoot/CaptureBuffers.ps1" -Mode $Mode -DebugView 815 -Label "$Label-authored-repeat"
    } else {
        & "$PSScriptRoot/CaptureBuffers.ps1" -Mode $Mode -DebugView 801 -Label "$Label-classification"
        & "$PSScriptRoot/CaptureBuffers.ps1" -Mode $Mode -DebugView 805 -Label "$Label-visibility"
    }
} finally {
    $null = Invoke-SkinTool 'communityshaders.remix' @{name='rtx.debugView.debugViewIdx';value='0'}
    if ($frozen -and (Invoke-SkinTool 'camera' @{action='get'}).freeCam) {
        $null = Invoke-SkinTool 'console' @{command='tfc 1'}
        Start-Sleep -Milliseconds 700
    }
    if ((Invoke-SkinTool 'camera' @{action='get'}).freeCam) { throw 'Free camera remains active; inspect before toggling again.' }
    $null = Invoke-SkinTool 'camera' @{action='setPov';pov='first'}
}
