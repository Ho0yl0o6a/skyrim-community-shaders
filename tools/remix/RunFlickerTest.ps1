param(
    # The pose the flicker reproduces at: standing inside the wall of the mill
    # building at the north end of Riverwood, in first person. Taken from the
    # save below, which was authored specifically to reproduce this.
    [double[]]$Position = @(19816.95, -46662.42, -145.22),
    # Facing matters as much as position. Inside the wall most directions look
    # at a solid surface and never show the bad frame at all; sweeping the
    # heading in 45-degree steps found the flash only at 90, where the view the
    # frame jumps to is what fills the screen. Every other heading measured a
    # largest step under 2.2 while this one measured 103.
    [double]$HeadingDegrees = 90,
    [double]$PitchDegrees = 6,
    [string]$Save = 'Save409_00000000_0_507269736F6E6572_Tamriel_000001_20260918093445_1_1',
    [int]$Frames = 14,
    [int]$Runs = 2,
    [string]$Label = 'flicker',
    [switch]$SkipLaunch,
    [switch]$SkipLoad
)
$ErrorActionPreference = 'Stop'
# Reproduces the white flicker seen when the camera is inside geometry, as a
# number rather than an impression.
#
# The camera does not move during measurement, so every frame should look the
# same. What the defect does is put an occasional frame on screen that was
# rendered from a different pass's camera or rebasing origin: the view jumps
# somewhere else entirely, and when what fills it is untextured distant terrain
# the frame reads as a white flash. A luminance step of ~100 between adjacent
# frames at a fixed pose is the signature; a healthy run holds within a few
# units.
function Invoke-DB([string]$Name, [string]$Body) {
    Invoke-RestMethod -Uri "http://127.0.0.1:8920/api/tool/$Name" -Method Post `
        -ContentType 'application/json' -Body $Body -TimeoutSec 30
}
if (!$SkipLaunch -and !(Get-Process SkyrimSE -ErrorAction SilentlyContinue)) {
    & (Join-Path $PSScriptRoot 'LaunchTest.ps1') | Write-Output
    & (Join-Path $PSScriptRoot 'ConfigureRunningTest.ps1') -Mode Scene -TimeoutSeconds 180 | Write-Output
}
if (!$SkipLoad) {
    $null = Invoke-DB 'game' (@{ action = 'load'; name = $Save } | ConvertTo-Json -Compress)
    $deadline = [DateTime]::UtcNow.AddSeconds(300)
    do {
        Start-Sleep -Seconds 5
        $state = $null
        try { $state = Invoke-DB 'inspect' '{"kind":"state"}' } catch { }
    } while (!$state.playerLoaded -and [DateTime]::UtcNow -lt $deadline)
    if (!$state.playerLoaded) { throw 'The repro save did not load.' }
}
# Pin the light. The flash is bright because the view it jumps to is lit by the
# sun; leaving the clock running changes how bright, and that would show up as
# run-to-run noise in exactly the number being measured.
$null = Invoke-DB 'console' '{"command":"set timescale to 0"}'
$null = Invoke-DB 'console' '{"command":"set gamehour to 12"}'
$null = Invoke-DB 'console' '{"command":"fw 81a"}'
$null = Invoke-DB 'console' (@{ command = ("player.setpos x {0}" -f $Position[0]) } | ConvertTo-Json -Compress)
$null = Invoke-DB 'console' (@{ command = ("player.setpos y {0}" -f $Position[1]) } | ConvertTo-Json -Compress)
$null = Invoke-DB 'console' (@{ command = ("player.setpos z {0}" -f $Position[2]) } | ConvertTo-Json -Compress)
$null = Invoke-DB 'console' (@{ command = ("player.setangle z {0}" -f $HeadingDegrees) } | ConvertTo-Json -Compress)
$null = Invoke-DB 'console' (@{ command = ("player.setangle x {0}" -f $PitchDegrees) } | ConvertTo-Json -Compress)
Start-Sleep -Seconds 12
$results = @()
foreach ($run in 1..$Runs) {
    $results += (& (Join-Path $PSScriptRoot 'MeasureFlicker.ps1') -Frames $Frames -Label ("{0}-run{1}" -f $Label, $run) | ConvertFrom-Json)
}
$steps = @($results | ForEach-Object { $_.largestStepBetweenFrames })
[ordered]@{
    runs = $results
    worstStep = ($steps | Measure-Object -Maximum).Maximum
    # A frame that jumps this far at a fixed pose is the defect. Below it the
    # run is ordinary path-tracer variance.
    flickering = (($steps | Measure-Object -Maximum).Maximum -gt 40)
} | ConvertTo-Json -Compress -Depth 5
