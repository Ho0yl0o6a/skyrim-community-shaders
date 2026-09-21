param([string]$Label = 'native-colour-flag')
$ErrorActionPreference = 'Stop'
function Invoke-DB([string]$Name, [hashtable]$Body) {
    Invoke-RestMethod -Uri "http://127.0.0.1:8920/api/tool/$Name" -Method Post -ContentType 'application/json' -Body ($Body | ConvertTo-Json -Compress) -TimeoutSec 10
}
function Set-Option([string]$Name, [string]$Value) {
    $result = Invoke-DB 'communityshaders.remix' @{name=$Name;value=$Value}
    if (!$result.success) { throw "Rejected $Name=$Value" }
}
$menu = Invoke-DB 'menu' @{action='list'}
if ($menu.messageBoxOpen -or $menu.openMenus -notcontains 'HUD Menu') { throw 'Loaded HUD without a modal required.' }
if ((Invoke-DB 'camera' @{action='get'}).freeCam) { throw 'Start outside free camera.' }
try {
    $null = Invoke-DB 'console' @{command='tfc 1'}
    Start-Sleep -Milliseconds 700
    if (!(Invoke-DB 'camera' @{action='get'}).freeCam) { throw 'Free camera did not engage.' }
    Set-Option 'rtx.debugView.debugViewIdx' '23'
    Set-Option 'rtx.opaqueMaterial.nativeAlbedoScale' '1'
    & "$PSScriptRoot/Capture.ps1" -Position 15133,-46900,650 -Pitch -0.15 -Yaw 0 -Label "$Label-scale1"
    Set-Option 'rtx.opaqueMaterial.nativeAlbedoScale' '0.5'
    & "$PSScriptRoot/Capture.ps1" -Label "$Label-scale05"
    Set-Option 'rtx.opaqueMaterial.nativeAlbedoScale' '1'
    & "$PSScriptRoot/Capture.ps1" -Label "$Label-scale1-return"
    Set-Option 'rtx.debugView.debugViewIdx' '0'
    & "$PSScriptRoot/Capture.ps1" -Label "$Label-lit" -SettleMs 3000
} finally {
    foreach ($pair in @(@('rtx.opaqueMaterial.nativeAlbedoScale','1'), @('rtx.debugView.debugViewIdx','0'))) {
        try { Set-Option $pair[0] $pair[1] } catch { Write-Warning $_ }
    }
    $null = Invoke-DB 'console' @{command='tfc 1'}
    Start-Sleep -Milliseconds 700
    Invoke-DB 'camera' @{action='get'} | ConvertTo-Json -Compress
}
