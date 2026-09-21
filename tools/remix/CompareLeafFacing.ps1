param([string]$Label = 'leaf-facing')
$ErrorActionPreference = 'Stop'
function Invoke-DB([string]$Name, [hashtable]$Body) {
    Invoke-RestMethod -Uri "http://127.0.0.1:8920/api/tool/$Name" -Method Post -ContentType 'application/json' -Body ($Body | ConvertTo-Json -Compress) -TimeoutSec 10
}
function Set-Option([string]$Name, [string]$Value) {
    if (!(Invoke-DB 'communityshaders.remix' @{name=$Name;value=$Value}).success) { throw "Rejected $Name=$Value" }
}
$menu = Invoke-DB 'menu' @{action='list'}
if ($menu.messageBoxOpen -or $menu.openMenus -notcontains 'HUD Menu') { throw 'Loaded HUD without a modal required.' }
if ((Invoke-DB 'camera' @{action='get'}).freeCam) { throw 'Start outside free camera.' }
$frozen = $false
try {
    $null = Invoke-DB 'console' @{command='tfc 1'}
    Start-Sleep -Milliseconds 700
    $frozen = (Invoke-DB 'camera' @{action='get'}).freeCam
    if (!$frozen) { throw 'Free camera did not engage.' }
    Set-Option 'rtx.useRTXDI' 'true'
    Set-Option 'rtx.debugView.debugViewIdx' '0'
    & "$PSScriptRoot/Capture.ps1" -Position 15133,-46900,650 -Pitch -0.15 -Yaw 0 -Label "$Label-lit" -SettleMs 3000
    foreach ($view in @(@('808','normal-facing'), @('800','thin-selection'), @('805','rtxdi-visibility'))) {
        Set-Option 'rtx.debugView.debugViewIdx' $view[0]
        & "$PSScriptRoot/Capture.ps1" -Label "$Label-$($view[1])"
    }
    Set-Option 'rtx.useRTXDI' 'false'
    & "$PSScriptRoot/Capture.ps1" -Label "$Label-ris-visibility"
    Set-Option 'rtx.debugView.debugViewIdx' '0'
    & "$PSScriptRoot/Capture.ps1" -Label "$Label-ris-lit" -SettleMs 3000
} finally {
    foreach ($pair in @(@('rtx.useRTXDI','true'), @('rtx.debugView.debugViewIdx','0'))) {
        try { Set-Option $pair[0] $pair[1] } catch { Write-Warning $_ }
    }
    if ($frozen -and (Invoke-DB 'camera' @{action='get'}).freeCam) {
        $null = Invoke-DB 'console' @{command='tfc 1'}
        Start-Sleep -Milliseconds 700
    }
    $camera = Invoke-DB 'camera' @{action='get'}
    if ($camera.freeCam) { throw 'Free camera still active; inspect before toggling again.' }
    $camera | ConvertTo-Json -Compress
}
