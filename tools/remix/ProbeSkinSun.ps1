param([int]$SettleMs = 1000)
$ErrorActionPreference = 'Stop'
function Invoke-DB([string]$Name, [hashtable]$Body) {
    Invoke-RestMethod -Uri "http://127.0.0.1:8920/api/tool/$Name" -Method Post -ContentType application/json -Body ($Body | ConvertTo-Json -Compress) -TimeoutSec 10
}
function Set-Probe([string]$Name, [string]$Value) {
    if (!(Invoke-DB 'communityshaders.remix' @{name=$Name;value=$Value}).success) { throw "Rejected $Name" }
}
if ((Invoke-DB 'camera' @{action='get'}).freeCam) { throw 'Start outside free camera.' }
$frozen = $false
try {
    $null = Invoke-DB 'camera' @{action='setPov';pov='third'}
    Start-Sleep -Milliseconds 700
    $null = Invoke-DB 'console' @{command='tfc 1'}
    Start-Sleep -Milliseconds 700
    $frozen = (Invoke-DB 'camera' @{action='get'}).freeCam
    if (!$frozen) { throw 'Freeze camera did not engage.' }
    $null = Invoke-DB 'camera' @{action='drive';x=13655;y=-48295;z=-170.8372;pitch=0.157043;yaw=-0.0800914}
    Set-Probe 'rtx.debugView.debugViewIdx' '813'
    Set-Probe 'rtx.debugView.gpuPrint.requireCtrl' 'False'
    Set-Probe 'rtx.debugView.gpuPrint.useMousePosition' 'False'
    Set-Probe 'rtx.debugView.gpuPrint.nativeEffectFlags' '65535'
    & "$PSScriptRoot/Capture.ps1" -Label 'skin-sun-probe-mask' -SettleMs 2000
    foreach ($pixel in @(@(760,140),@(800,180),@(830,140),@(970,120))) {
        Set-Probe 'rtx.debugView.gpuPrint.enable' 'False'
        Set-Probe 'rtx.debugView.gpuPrint.pixelIndex' "$($pixel[0]),$($pixel[1])"
        $start = [DateTime]::Now
        Set-Probe 'rtx.debugView.gpuPrint.enable' 'True'
        Start-Sleep -Milliseconds $SettleMs
        $records = foreach ($line in (Get-Content "$PSScriptRoot/../../.research/testlogs/remix-dxvk.log" -Tail 1000)) {
            if ($line -match '^\[(?<time>\d\d:\d\d:\d\d\.\d{3})\].*Frame: (?<frame>\d+) - GPU print value \[(?<x>\d+), (?<y>\d+)\]: (?<data>.*)$') {
                $stamp = $start.Date + [TimeSpan]::Parse($Matches.time)
                if ($stamp -ge $start -and [int]$Matches.x -eq $pixel[0] -and [int]$Matches.y -eq $pixel[1]) {
                    [ordered]@{frame=[uint32]$Matches.frame;value=$Matches.data}
                }
            }
        }
        [ordered]@{pixel=$pixel;fields='geometryDotSun,shadingDotSun,geometryDotShading,distantLightCount';records=@($records | Select-Object -Last 8)} | ConvertTo-Json -Depth 4 -Compress
    }
} finally {
    foreach ($setting in @(@('enable','False'),@('nativeEffectFlags','-1'),@('requireCtrl','True'),@('useMousePosition','True'))) {
        try { Set-Probe "rtx.debugView.gpuPrint.$($setting[0])" $setting[1] } catch { Write-Warning $_ }
    }
    Set-Probe 'rtx.debugView.debugViewIdx' '0'
    if ($frozen -and (Invoke-DB 'camera' @{action='get'}).freeCam) {
        $null = Invoke-DB 'console' @{command='tfc 1'}
        Start-Sleep -Milliseconds 700
    }
    if ((Invoke-DB 'camera' @{action='get'}).freeCam) { throw 'Free camera remains active.' }
    $null = Invoke-DB 'camera' @{action='setPov';pov='first'}
}
