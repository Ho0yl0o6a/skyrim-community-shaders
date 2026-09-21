param(
    [string]$Label = 'material-grazing',
    [int[]]$Views = @(0, 808, 15, 16, 805, 0),
    [ValidateSet('Screenshots', 'Pair')][string]$Mode = 'Screenshots',
    [ValidateCount(3,3)][double[]]$Position,
    [double]$Pitch = 0,
    [double]$Yaw = 0
)
$ErrorActionPreference = 'Stop'
function Invoke-MaterialTool([string]$Name, [hashtable]$Body) {
    Invoke-RestMethod "http://127.0.0.1:8920/api/tool/$Name" -Method Post -ContentType application/json -Body ($Body | ConvertTo-Json -Compress) -TimeoutSec 10
}
function Set-MaterialView([int]$Index) {
    if (!(Invoke-MaterialTool 'communityshaders.remix' @{name='rtx.debugView.debugViewIdx';value="$Index"}).success) {
        throw "Rejected debug view $Index"
    }
}
if ((Invoke-MaterialTool 'camera' @{action='get'}).freeCam) { throw 'Start outside free camera.' }
$frozen = $false
try {
    $null = Invoke-MaterialTool 'console' @{command='tfc 1'}
    Start-Sleep -Milliseconds 700
    $reference = Invoke-MaterialTool 'camera' @{action='get'}
    $frozen = $reference.freeCam
    if (!$frozen) { throw 'Freeze did not engage.' }
    if ($Position) {
        $null = Invoke-MaterialTool 'camera' @{action='drive';x=$Position[0];y=$Position[1];z=$Position[2];pitch=$Pitch;yaw=$Yaw}
        Start-Sleep -Milliseconds 700
        $reference = Invoke-MaterialTool 'camera' @{action='get'}
    }
    $ordinal = 0
    foreach ($view in $Views) {
        Set-MaterialView $view
        if ($Mode -eq 'Pair') {
            $capture = & "$PSScriptRoot/CaptureBuffers.ps1" -Mode Pair -DebugView $view -Label "$Label-$ordinal-$view"
        } else {
            $capture = & "$PSScriptRoot/Capture.ps1" -NoFreecam -Label "$Label-$view" -SettleMs 1800
        }
        $ordinal++
        $capture
        $observed = ($capture | ConvertFrom-Json).camera
        foreach ($field in @('camX','camY','camZ','camPitch','camYaw')) {
            if ([Math]::Abs([double]$observed.$field - [double]$reference.$field) -gt 0.001) {
                throw "Camera moved during capture ($field); reject comparison."
            }
        }
    }
} finally {
    try { Set-MaterialView 0 }
    finally {
        if ($frozen -and (Invoke-MaterialTool 'camera' @{action='get'}).freeCam) {
            $null = Invoke-MaterialTool 'console' @{command='tfc 1'}
        }
    }
}
