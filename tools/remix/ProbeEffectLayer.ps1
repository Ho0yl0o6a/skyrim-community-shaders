param(
    [Parameter(Mandatory=$true)][ValidateRange(0,65535)][int]$Flags,
    [Parameter(Mandatory=$true)][ValidateRange(0,16384)][int]$X,
    [Parameter(Mandatory=$true)][ValidateRange(0,16384)][int]$Y,
    [ValidateRange(200,2000)][int]$SettleMs = 500,
    [string]$Log = (Join-Path $PSScriptRoot '../../.research/testlogs/remix-dxvk.log')
)
$ErrorActionPreference = 'Stop'
function Set-Probe([string]$Name, [string]$Value) {
    $result = Invoke-RestMethod http://127.0.0.1:8920/api/tool/communityshaders.remix -Method Post -ContentType application/json -TimeoutSec 10 -Body (@{name="rtx.debugView.gpuPrint.$Name";value=$Value}|ConvertTo-Json -Compress)
    if (!$result.success) { throw "Probe setting rejected: $Name" }
}
if (!(Test-Path -LiteralPath $Log)) { throw 'No runtime log found.' }
# Session-only diagnostic. Keep camera still; coordinates are internal render
# pixels, not output pixels. More than one matching layer can still overwrite.
$names = @('bindings','sourceRGBA','resolvedRGBA','UV','vertexRGBA','alphaFactors','emissionAndOpacity','blendClassification')
$rows = @()
try {
    Set-Probe enable False
    Set-Probe nativeEffectFlags "$Flags"
    Set-Probe requireCtrl False
    Set-Probe useMousePosition False
    Set-Probe pixelIndex "$X,$Y"
    for ($phase=0; $phase -lt 8; ++$phase) {
        $start = [DateTime]::Now
        Set-Probe nativeEffectPhase "$phase"
        Set-Probe enable True
        Start-Sleep -Milliseconds $SettleMs
        # The timestamps distinguish this phase from earlier lines for the same
        # pixel. Allow no stale values when a filtered layer was not hit.
        $records = foreach ($line in (Get-Content -LiteralPath $Log -Tail 600)) {
            if ($line -match '^\[(?<time>\d\d:\d\d:\d\d\.\d{3})\].*Frame: (?<frame>\d+) - GPU print value \[(?<x>\d+), (?<y>\d+)\]: (?<data>.*)$') {
                $stamp = $start.Date + [TimeSpan]::Parse($Matches.time)
                if ($stamp -ge $start -and [int]$Matches.x -eq $X -and [int]$Matches.y -eq $Y) {
                    [ordered]@{frame=[uint32]$Matches.frame;value=$Matches.data}
                }
            }
        }
        $rows += [ordered]@{phase=$phase;name=$names[$phase];records=@($records | Select-Object -Last 4)}
        Set-Probe enable False
    }
} finally {
    foreach ($setting in @(@('enable','False'),@('nativeEffectFlags','-1'),@('nativeEffectPhase','-1'),@('requireCtrl','True'),@('useMousePosition','True'))) {
        try { Set-Probe $setting[0] $setting[1] } catch { Write-Warning "Failed to restore probe $($setting[0]): $_" }
    }
}
[ordered]@{flags=$Flags;pixel=@($X,$Y);phases=$rows;
    scope='GPU values for the last matching native effect at the selected internal pixel. No hit identity, sorted-layer count, native raster comparison or full image parity is implied.'} | ConvertTo-Json -Depth 7
