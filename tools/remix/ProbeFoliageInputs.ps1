param(
    [Parameter(Mandatory=$true)][ValidateRange(0,16384)][int]$X,
    [Parameter(Mandatory=$true)][ValidateRange(0,16384)][int]$Y,
    [int]$SettleMs = 1000,
    [string]$Log = (Join-Path $PSScriptRoot '../../.research/testlogs/remix-dxvk.log')
)
$ErrorActionPreference = 'Stop'
function Set-Probe([string]$Name, [string]$Value) {
    $result = Invoke-RestMethod http://127.0.0.1:8920/api/tool/communityshaders.remix -Method Post -ContentType application/json -TimeoutSec 10 -Body (@{name=$Name;value=$Value}|ConvertTo-Json -Compress)
    if (!$result.success) { throw "Probe setting rejected: $Name" }
}
# Pixel coordinates are internal render pixels. Debug809 cycles w=0 albedo,
# w=1 soft map, w=2 linear soft product, w=3 rolloff/gamma/diffuse multiplier.
try {
    Set-Probe 'rtx.debugView.debugViewIdx' '809'
    Set-Probe 'rtx.debugView.gpuPrint.enable' 'False'
    Set-Probe 'rtx.debugView.gpuPrint.requireCtrl' 'False'
    Set-Probe 'rtx.debugView.gpuPrint.useMousePosition' 'False'
    Set-Probe 'rtx.debugView.gpuPrint.nativeEffectFlags' '65535'
    Set-Probe 'rtx.debugView.gpuPrint.pixelIndex' "$X,$Y"
    $start = [DateTime]::Now
    Set-Probe 'rtx.debugView.gpuPrint.enable' 'True'
    Start-Sleep -Milliseconds $SettleMs
    $records = foreach ($line in (Get-Content -LiteralPath $Log -Tail 1000)) {
        if ($line -match '^\[(?<time>\d\d:\d\d:\d\d\.\d{3})\].*Frame: (?<frame>\d+) - GPU print value \[(?<x>\d+), (?<y>\d+)\]: (?<data>.*)$') {
            $stamp = $start.Date + [TimeSpan]::Parse($Matches.time)
            if ($stamp -ge $start -and [int]$Matches.x -eq $X -and [int]$Matches.y -eq $Y) {
                [ordered]@{frame=[uint32]$Matches.frame;value=$Matches.data}
            }
        }
    }
    [ordered]@{pixel=@($X,$Y);records=@($records | Select-Object -Last 16);scope='Selected native non-grass hit inputs only; no raster-parity verdict.'} | ConvertTo-Json -Depth 5 -Compress
} finally {
    foreach ($setting in @(@('enable','False'),@('nativeEffectFlags','-1'),@('requireCtrl','True'),@('useMousePosition','True'))) {
        try { Set-Probe "rtx.debugView.gpuPrint.$($setting[0])" $setting[1] } catch { Write-Warning $_ }
    }
    Set-Probe 'rtx.debugView.debugViewIdx' '0'
}
