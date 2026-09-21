param(
    [uint32[]]$MinPrims = @(1000, 300, 100),
    [int]$MeasureSeconds = 10,
    [int]$SettleSeconds = 45
)
$ErrorActionPreference = 'Stop'
# Measures the merged-bucket trade-off from a cold launch per case, because the
# routing decision is taken once per geometry and then pinned: a mesh that has
# been given its own acceleration structure keeps it, so the option has to be in
# place before the cell loads. Changes only process-local Remix options.
function Invoke-DB([string]$Name, [string]$Body) {
    Invoke-RestMethod -Uri "http://127.0.0.1:8920/api/tool/$Name" -Method Post `
        -ContentType 'application/json' -Body $Body -TimeoutSec 25
}
function Set-Remix([string]$Name, [string]$Value) {
    $result = Invoke-DB 'communityshaders.remix' (@{ name = $Name; value = $Value } | ConvertTo-Json -Compress)
    if (!$result.success) { throw "Remix rejected $Name=$Value" }
}
foreach ($threshold in $MinPrims) {
    Get-Process SkyrimSE -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Seconds 5
    & (Join-Path $PSScriptRoot 'LaunchTest.ps1') | Out-Null
    & (Join-Path $PSScriptRoot 'ConfigureRunningTest.ps1') -Mode Scene -TimeoutSeconds 180 | Out-Null
    Set-Remix 'rtx.mergeRetainedInstances' 'True'
    Set-Remix 'rtx.minPrimsInDynamicBLAS' "$threshold"
    $null = Invoke-DB 'console' '{"command":"coc riverwood"}'
    $result = & (Join-Path $PSScriptRoot 'RunRiverwoodTest.ps1') -SkipLaunch -SettleSeconds $SettleSeconds -MeasureSeconds $MeasureSeconds | ConvertFrom-Json
    [pscustomobject]@{ minPrimsInDynamicBLAS = $threshold; fps = $result.frameRate }
}
