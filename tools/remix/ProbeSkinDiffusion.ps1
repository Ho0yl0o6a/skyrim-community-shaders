param([string]$Label = 'skin-diffusion-distance')
$ErrorActionPreference = 'Stop'
function Invoke-SkinTool([string]$Name, [hashtable]$Body) {
    Invoke-RestMethod "http://127.0.0.1:8920/api/tool/$Name" -Method Post -ContentType application/json -Body ($Body | ConvertTo-Json -Compress) -TimeoutSec 10
}
function Set-Probe([string]$Name, [string]$Value) {
    if (!(Invoke-SkinTool 'communityshaders.remix' @{name=$Name;value=$Value}).success) { throw "Rejected $Name" }
}
if ((Invoke-SkinTool 'camera' @{action='get'}).freeCam) { throw 'Start outside free camera.' }
$frozen = $false
try {
    $null = Invoke-SkinTool 'console' @{command='tfc 1'}
    Start-Sleep -Milliseconds 700
    $frozen = (Invoke-SkinTool 'camera' @{action='get'}).freeCam
    if (!$frozen) { throw 'Freeze did not engage.' }
    $null = Invoke-SkinTool 'camera' @{action='drive';x=13620;y=-48240;z=-158;pitch=0;yaw=1.171622}
    Set-Probe 'rtx.debugView.debugViewIdx' '816'
    Set-Probe 'rtx.debugView.gpuPrint.requireCtrl' 'false'
    Set-Probe 'rtx.debugView.gpuPrint.useMousePosition' 'false'
    & "$PSScriptRoot/Capture.ps1" -NoFreecam -Label $Label -SettleMs 2000
    foreach ($pixel in @(@(480,400),@(500,500),@(600,360),@(700,500))) {
        Set-Probe 'rtx.debugView.gpuPrint.enable' 'false'
        Set-Probe 'rtx.debugView.gpuPrint.pixelIndex' "$($pixel[0]),$($pixel[1])"
        $start = [DateTime]::Now
        Set-Probe 'rtx.debugView.gpuPrint.enable' 'true'
        Start-Sleep -Milliseconds 1200
        $records = foreach ($line in (Get-Content "$PSScriptRoot/../../.research/testlogs/remix-dxvk.log" -Tail 900)) {
            if ($line -match '^\[(?<time>\d\d:\d\d:\d\d\.\d{3})\].*Frame: (?<frame>\d+) - GPU print value \[(?<x>\d+), (?<y>\d+)\]: (?<data>.*)$') {
                $stamp = $start.Date + [TimeSpan]::Parse($Matches.time)
                if ($stamp -ge $start -and [int]$Matches.x -eq $pixel[0] -and [int]$Matches.y -eq $pixel[1]) {
                    [ordered]@{frame=[uint32]$Matches.frame;value=$Matches.data}
                }
            }
        }
        [ordered]@{pixel=$pixel;fields='actualDistance,projectedDistance,maxWeight,normalAlignment';records=@($records)} | ConvertTo-Json -Depth 4 -Compress
    }
} finally {
    foreach ($setting in @(@('enable','false'),@('requireCtrl','true'),@('useMousePosition','true'))) {
        try { Set-Probe "rtx.debugView.gpuPrint.$($setting[0])" $setting[1] } catch { Write-Warning $_ }
    }
    try { Set-Probe 'rtx.debugView.debugViewIdx' '0' }
    finally {
        if ($frozen -and (Invoke-SkinTool 'camera' @{action='get'}).freeCam) {
            $null = Invoke-SkinTool 'console' @{command='tfc 1'}
        }
    }
}
