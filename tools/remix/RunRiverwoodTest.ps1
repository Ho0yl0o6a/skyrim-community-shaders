param(
    [double[]]$Position = @(20480, -45670, 300),
    [double]$Pitch = 0.1,
    [double]$Yaw = 0.2,
    # Fallback only. Settling is decided by measurement, not by the clock: see
    # Wait-ForSteadyState. A fixed wait was tried at 20 and then 45 seconds and
    # read two and then five frames a second low, because shader and pipeline
    # compilation, micromap baking and the radiance cache's training all keep
    # going long after the camera stops -- and for a variable time.
    [int]$SettleSeconds = 45,
    [int]$SettleTimeoutSeconds = 420,
    [int]$MeasureSeconds = 10,
    [switch]$SkipLaunch,
    [switch]$NoMeasure
)
$ErrorActionPreference = 'Stop'
# Launches (unless already running), loads Riverwood, parks the free camera at a
# fixed pose and reports the frame rate with the host-side CPU phase costs.
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
function Invoke-DB([string]$Name, [string]$Body) {
    Invoke-RestMethod -Uri "http://127.0.0.1:8920/api/tool/$Name" -Method Post `
        -ContentType 'application/json' -Body $Body -TimeoutSec 25
}
if (!$SkipLaunch -and !(Get-Process SkyrimSE -ErrorAction SilentlyContinue)) {
    & (Join-Path $PSScriptRoot 'LaunchTest.ps1') | Write-Output
    & (Join-Path $PSScriptRoot 'ConfigureRunningTest.ps1') -Mode Scene -LoadRiverwood -TimeoutSeconds 180 | Write-Output
}
$deadline = [DateTime]::UtcNow.AddSeconds(240)
do {
    Start-Sleep -Seconds 3
    $state = $null
    try { $state = Invoke-DB 'inspect' '{"kind":"state"}' } catch { }
} while (!$state.playerLoaded -and [DateTime]::UtcNow -lt $deadline)
if (!$state.playerLoaded) { throw 'Riverwood did not load.' }
Start-Sleep -Seconds 4
# A message box pauses the world, so leaving one up would measure a frame with
# no animation, no skinning and no acceleration-structure refits -- a number
# that has nothing to do with the one the goal asks for. Boxes can also appear
# after the load finishes (the Survival Mode prompt does), so keep dismissing
# until none is left, and decline rather than accept: "Yes" on that prompt
# changes the save's gameplay rules.
function Close-MessageBoxes {
    for ($attempt = 0; $attempt -lt 8; $attempt++) {
        $modal = Invoke-DB 'menu' '{"action":"describe"}'
        if (!$modal.messageBoxOpen) { return }
        $buttons = @($modal.buttons)
        $decline = [Array]::FindIndex([string[]]$buttons, [Predicate[string]] { param($b) $b -match '^(No|Cancel|Decline)$' })
        if ($decline -lt 0) { throw "No safe decline button on modal: $($modal.bodyText)" }
        $null = Invoke-DB 'menu' (@{ action = 'accept'; index = $decline } | ConvertTo-Json -Compress)
        Start-Sleep -Seconds 2
    }
    throw 'A message box stayed open; the world would be paused during measurement.'
}
Close-MessageBoxes
$camera = Invoke-DB 'camera' '{"action":"get"}'
if (!$camera.freeCam) {
    $null = Invoke-DB 'camera' '{"action":"freecam","on":true}'
    $wait = [DateTime]::UtcNow.AddSeconds(10)
    do { Start-Sleep -Milliseconds 300; $camera = Invoke-DB 'camera' '{"action":"get"}' } while (!$camera.freeCam -and [DateTime]::UtcNow -lt $wait)
}
$null = Invoke-DB 'camera' (@{ action = 'drive'; x = $Position[0]; y = $Position[1]; z = $Position[2]; pitch = $Pitch; yaw = $Yaw } | ConvertTo-Json -Compress)
# Pin the conditions the frame cost depends on. The save loads at whatever hour
# and weather it was saved in and then keeps running: sun angle, cloud cover and
# the number of lit surfaces all drift while the camera sits still, which showed
# up as +/- 2 FPS between otherwise identical runs and made single-run A/B
# comparisons worthless. Stopping the clock and forcing one weather makes
# successive runs comparable. Process-local console state only; nothing is
# saved.
$null = Invoke-DB 'console' '{"command":"set timescale to 0"}'
$null = Invoke-DB 'console' '{"command":"set gamehour to 12"}'
$null = Invoke-DB 'console' '{"command":"fw 81a"}'
# Wait until the frame rate stops climbing rather than for a fixed time. The
# scene keeps getting faster for MINUTES after the camera stops -- shaders and
# pipelines compile, opacity micromaps bake, the radiance cache trains -- and
# how long it takes varies with what is already cached on disk.
#
# The slope is what makes this awkward: the climb is slow and steady rather than
# noisy, so any test that only looks at neighbouring windows sees a flat line and
# declares victory. Comparing three five-second windows against their immediate
# neighbours passed after twenty seconds at 43 FPS in a scene that reaches 53.
# So compare against a window a full minute earlier instead: a climb of a couple
# of frames a second per minute is invisible locally and obvious over that span.
function Wait-ForSteadyState {
    param(
        [int]$TimeoutSeconds,
        [int]$WindowSeconds = 5,
        # Improvement across the lookback below which the scene counts as settled.
        [double]$Tolerance = 0.01,
        # How far back to compare. Twelve windows is a minute.
        [int]$LookbackWindows = 12)
    $windows = [Collections.Generic.List[double]]::new()
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $previous = Invoke-DB 'inspect' '{"kind":"state"}'
    $previousAt = [DateTime]::UtcNow
    while ([DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Seconds $WindowSeconds
        $current = Invoke-DB 'inspect' '{"kind":"state"}'
        $at = [DateTime]::UtcNow
        $elapsed = ($at - $previousAt).TotalSeconds
        if ($current.pid -ne $previous.pid -or $current.frame -lt $previous.frame) { throw 'Game restarted while settling.' }
        $windows.Add(($current.frame - $previous.frame) / $elapsed)
        $previous = $current
        $previousAt = $at
        if ($windows.Count -lt ($LookbackWindows + 3)) { continue }
        $last = $windows.Count - 1
        $recent = @($windows[($last - 2)..$last])
        $earlier = @($windows[($last - $LookbackWindows - 2)..($last - $LookbackWindows)])
        $recentMean = ($recent | Measure-Object -Average).Average
        $earlierMean = ($earlier | Measure-Object -Average).Average
        if ($earlierMean -le 0) { continue }
        $gain = ($recentMean - $earlierMean) / $earlierMean
        if ($gain -le $Tolerance) {
            return [ordered]@{
                settledSeconds = [Math]::Round($windows.Count * $WindowSeconds, 1)
                settledFps = [Math]::Round($recentMean, 2)
                gainOverLastMinute = [Math]::Round($gain * 100, 2)
                windows = @($windows | ForEach-Object { [Math]::Round($_, 2) })
                settled = $true
            }
        }
    }
    $tail = if ($windows.Count) { ($windows[($windows.Count - 1)]) } else { 0 }
    return [ordered]@{
        settledSeconds = [Math]::Round($windows.Count * $WindowSeconds, 1)
        settledFps = [Math]::Round($tail, 2)
        windows = @($windows | ForEach-Object { [Math]::Round($_, 2) })
        settled = $false
    }
}
$settle = Wait-ForSteadyState -TimeoutSeconds $SettleTimeoutSeconds
if (!$settle.settled) {
    Write-Warning "Frame rate had not settled after $($settle.settledSeconds)s; measuring anyway."
    Start-Sleep -Seconds $SettleSeconds
}
Close-MessageBoxes
if (!$NoMeasure) {
    $measurement = & (Join-Path $PSScriptRoot 'MeasureRunningTest.ps1') -Seconds $MeasureSeconds | ConvertFrom-Json
    $log = "$env:USERPROFILE\OneDrive\Documents\My Games\Skyrim Special Edition\SKSE\CommunityShaders.log"
    $cpu = (Select-String -Path $log -Pattern 'RemixScene\.cpu\] median ms' | Select-Object -Last 1).Line
    $ineligible = (Select-String -Path $log -Pattern 'RemixScene\.cpu\] ineligible' | Select-Object -Last 1).Line
    $dlfg = (Select-String -Path 'J:\hdresreach\skyrim-community-shaders\.research\testlogs\remix-dxvk.log' -Pattern '\[Perf\.Dlfg\]' | Select-Object -Last 1).Line
    [ordered]@{
        pid = $measurement.pid
        frameRate = $measurement.frameRate
        settle = $settle
        presented = $dlfg
        gpuFrameMedianMs = $measurement.gpu.'frame.totalMs'.medianMs
        gpuSceneMedianMs = $measurement.gpu.'frame.scene'.medianMs
        gpuPathTracingMedianMs = $measurement.gpu.'frame.pathTracing'.medianMs
        gpuIndirectMedianMs = $measurement.gpu.'integrate.indirect'.medianMs
        cpu = $cpu
        ineligible = $ineligible
    } | ConvertTo-Json -Compress
}
