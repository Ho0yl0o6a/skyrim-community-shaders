param(
    [Parameter(Mandatory = $true)][string[]]$Settings,
    [int]$Seconds = 8,
    [int]$SettleSeconds = 5
)
$ErrorActionPreference = 'Stop'
# Measures the running test process with each "name=value[;name=value...]" case,
# restoring the value each case started from. It changes only process-local Remix
# options and never writes saved settings or game files.
function Set-Remix([string]$Name, [string]$Value) {
    $result = Invoke-RestMethod -Uri 'http://127.0.0.1:8920/api/tool/communityshaders.remix' -Method Post `
        -ContentType 'application/json' -Body (@{ name = $Name; value = $Value } | ConvertTo-Json -Compress) -TimeoutSec 15
    if (!$result.success) { throw "Remix rejected $Name=$Value" }
}
function Measure-Case {
    $measurement = & (Join-Path $PSScriptRoot 'MeasureRunningTest.ps1') -Seconds $Seconds | ConvertFrom-Json
    [ordered]@{
        fps = $measurement.frameRate
        gpuMs = $measurement.gpu.'frame.totalMs'.medianMs
        sceneMs = $measurement.gpu.'frame.scene'.medianMs
        pathMs = $measurement.gpu.'frame.pathTracing'.medianMs
        indirectMs = $measurement.gpu.'integrate.indirect'.medianMs
        gbufferMs = $measurement.gpu.'pathTracing.gbuffer'.medianMs
    }
}
Start-Sleep -Seconds $SettleSeconds
$baseline = Measure-Case
$rows = @([ordered]@{ case = 'baseline' } + $baseline)
foreach ($case in $Settings) {
    foreach ($assignment in ($case -split ';')) {
        $pair = $assignment -split '=', 2
        Set-Remix $pair[0] $pair[1]
    }
    Start-Sleep -Seconds $SettleSeconds
    $rows += [ordered]@{ case = $case } + (Measure-Case)
}
$rows | ForEach-Object { [pscustomobject]$_ } | Format-Table -AutoSize | Out-String -Width 200
Write-Output 'Restore the options you want to keep explicitly; this script leaves the last case applied.'
