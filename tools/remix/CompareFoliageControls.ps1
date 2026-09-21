param([string]$Label = 'foliage-live-controls')
$ErrorActionPreference = 'Stop'
# Process-local diagnostic only. Requires a loaded, non-modal test scene.
function Invoke-DB([string]$Name, [hashtable]$Body) {
    Invoke-RestMethod -Uri "http://127.0.0.1:8920/api/tool/$Name" -Method Post -ContentType 'application/json' -Body ($Body | ConvertTo-Json -Compress) -TimeoutSec 10
}
function Set-Option([string]$Name, [string]$Value) {
    $result = Invoke-DB 'communityshaders.remix' @{name=$Name;value=$Value}
    if (!$result.success) { throw "Rejected $Name=$Value" }
}
function Capture([string]$Suffix) {
    & "$PSScriptRoot/Capture.ps1" -Label "$Label-$Suffix" -SettleMs 2000
}
$menu = Invoke-DB 'menu' @{action='list'}
if ($menu.messageBoxOpen -or $menu.openMenus -notcontains 'HUD Menu') { throw 'Loaded HUD without a modal required.' }
$camera = Invoke-DB 'camera' @{action='get'}
if ($camera.freeCam) { throw 'Start outside free camera so the freeze toggle has a known state.' }
$frozen = $false
try {
    $null = Invoke-DB 'console' @{command='tfc 1'}
    $frozen = $true
    Start-Sleep -Milliseconds 700
    if (!(Invoke-DB 'camera' @{action='get'}).freeCam) { throw 'Free camera did not engage.' }
    Set-Option 'rtx.subsurface.enableThinOpaque' 'true'
    Set-Option 'rtx.subsurface.enableTextureMaps' 'true'
    Set-Option 'rtx.debugView.debugViewIdx' '0'
    & "$PSScriptRoot/Capture.ps1" -Position 15133,-46900,650 -Pitch -0.15 -Yaw 0 -Label "$Label-authored-lit" -SettleMs 3000
    Set-Option 'rtx.subsurface.enableTextureMaps' 'false'
    Capture 'constant-lit'
    Set-Option 'rtx.debugView.debugViewIdx' '803'
    Capture 'constant-ssa'
    Set-Option 'rtx.subsurface.enableTextureMaps' 'true'
    Capture 'authored-ssa'
    Set-Option 'rtx.subsurface.enableThinOpaque' 'false'
    Set-Option 'rtx.debugView.debugViewIdx' '800'
    Capture 'thin-off-selection'
    Set-Option 'rtx.debugView.debugViewIdx' '0'
    Capture 'thin-off-lit'
    Set-Option 'rtx.subsurface.enableThinOpaque' 'true'
    Set-Option 'rtx.debugView.debugViewIdx' '800'
    Capture 'thin-on-selection'
} finally {
    # Restore independently so one rejected request cannot skip camera cleanup.
    foreach ($pair in @(
        @('rtx.subsurface.enableTextureMaps','true'),
        @('rtx.subsurface.enableThinOpaque','true'),
        @('rtx.debugView.debugViewIdx','0'))) {
        try { Set-Option $pair[0] $pair[1] } catch { Write-Warning $_ }
    }
    if ($frozen) {
        $null = Invoke-DB 'console' @{command='tfc 1'}
        Start-Sleep -Milliseconds 700
        $camera = Invoke-DB 'camera' @{action='get'}
        if ($camera.freeCam) { throw 'Free camera did not disengage; inspect before another toggle.' }
    }
}
