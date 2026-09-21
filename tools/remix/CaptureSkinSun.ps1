param([string]$Label = 'skin-sun')
$ErrorActionPreference = 'Stop'
function Invoke-DB([string]$Name, [hashtable]$Body) {
    Invoke-RestMethod -Uri "http://127.0.0.1:8920/api/tool/$Name" -Method Post -ContentType application/json -Body ($Body | ConvertTo-Json -Compress) -TimeoutSec 10
}
function Set-Debug([int]$Index) {
    if (!(Invoke-DB 'communityshaders.remix' @{name='rtx.debugView.debugViewIdx';value="$Index"}).success) {
        throw "Debug view $Index unavailable."
    }
}
if ((Invoke-DB 'camera' @{action='get'}).freeCam) { throw 'Start outside free camera for a known freeze state.' }
$frozen = $false
try {
    $null = Invoke-DB 'camera' @{action='setPov';pov='third'}
    Start-Sleep -Milliseconds 700
    $null = Invoke-DB 'console' @{command='tfc 1'}
    Start-Sleep -Milliseconds 700
    $frozen = (Invoke-DB 'camera' @{action='get'}).freeCam
    if (!$frozen) { throw 'Freeze camera did not engage.' }
    $null = Invoke-DB 'camera' @{action='drive';x=13655;y=-48295;z=-170.8372;pitch=0.157043;yaw=-0.0800914}
    foreach ($view in @(0,813,814,15,16,806,0)) {
        Set-Debug $view
        & "$PSScriptRoot/Capture.ps1" -Label "$Label-$view" -SettleMs 2000
    }
} finally {
    try { Set-Debug 0 } catch { Write-Warning $_ }
    if ($frozen -and (Invoke-DB 'camera' @{action='get'}).freeCam) {
        $null = Invoke-DB 'console' @{command='tfc 1'}
        Start-Sleep -Milliseconds 700
    }
    $camera = Invoke-DB 'camera' @{action='get'}
    if ($camera.freeCam) { throw 'Free camera remains active; inspect before toggling again.' }
    $null = Invoke-DB 'camera' @{action='setPov';pov='first'}
    $camera | ConvertTo-Json -Compress
}
