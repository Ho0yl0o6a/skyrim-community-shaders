param(
    [int]$Frames = 16,
    [int]$IntervalMs = 160,
    [string]$OutDir = 'J:/hdresreach/skyrim-community-shaders/.research/flicker',
    [string]$Label = 'flicker',
    [switch]$KeepFrames
)
$ErrorActionPreference = 'Stop'
# Captures a run of frames at whatever pose the game is already in and reports
# the mean luminance of each, so "the screen flickers" becomes a number.
#
# Frame size on disk is not a usable signal: a blown-out white frame and a black
# frame both compress to almost nothing, so a flicker in either direction looks
# the same as a plain dark scene. Mean luminance separates them and gives a
# direction. The camera is not moved here -- park it first; a fixed pose means
# any swing between consecutive frames is the defect rather than the view.
Add-Type -AssemblyName System.Drawing
function Invoke-DB([string]$Name, [string]$Body) {
    Invoke-RestMethod -Uri "http://127.0.0.1:8920/api/tool/$Name" -Method Post `
        -ContentType 'application/json' -Body $Body -TimeoutSec 25
}
function Get-MeanLuminance([string]$Path) {
    $bitmap = [System.Drawing.Bitmap]::FromFile($Path)
    try {
        # Sample a grid rather than every pixel: 1920x1080 per-pixel through
        # GetPixel takes minutes, and a 96x54 grid settles a mean to well inside
        # the swing being looked for.
        $stepX = [Math]::Max(1, [int]($bitmap.Width / 96))
        $stepY = [Math]::Max(1, [int]($bitmap.Height / 54))
        $total = 0.0
        $count = 0
        for ($y = 0; $y -lt $bitmap.Height; $y += $stepY) {
            for ($x = 0; $x -lt $bitmap.Width; $x += $stepX) {
                $pixel = $bitmap.GetPixel($x, $y)
                $total += (0.2126 * $pixel.R + 0.7152 * $pixel.G + 0.0722 * $pixel.B)
                $count++
            }
        }
        if ($count -eq 0) { return 0.0 }
        return [Math]::Round($total / $count, 2)
    } finally {
        $bitmap.Dispose()
    }
}
$null = New-Item -ItemType Directory -Force -Path $OutDir
$before = @((Invoke-DB 'inspect' (@{kind='screenshots';limit=($Frames+4)} | ConvertTo-Json -Compress)).screenshots)
$baselinePaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($shot in $before) { $null = $baselinePaths.Add($shot.path) }
$initialState = Invoke-DB 'inspect' '{"kind":"state"}'
$initialCamera = Invoke-DB 'camera' '{"action":"get"}'
$menus = Invoke-DB 'menu' '{"action":"list"}'
if (!$initialState.playerLoaded -or $menus.messageBoxOpen -or $menus.openMenus -contains 'Loading Menu') {
    throw 'Flicker capture needs a loaded, non-modal world.'
}
$captured = [Collections.Generic.List[string]]::new()
for ($i = 0; $i -lt $Frames; $i++) {
    $null = Invoke-DB 'communityshaders.capture' '{"kind":"screenshot"}'
    Start-Sleep -Milliseconds $IntervalMs
}
$deadline = [DateTime]::UtcNow.AddSeconds(30)
do {
    Start-Sleep -Milliseconds 500
    $shots = @((Invoke-DB 'inspect' (@{ kind = 'screenshots'; limit = ($Frames + 4) } | ConvertTo-Json -Compress)).screenshots)
    $fresh = @($shots | Where-Object { !$baselinePaths.Contains($_.path) } | Select-Object -First $Frames)
} while ($fresh.Count -lt $Frames -and [DateTime]::UtcNow -lt $deadline)
if ($fresh.Count -ne $Frames) { throw "Only $($fresh.Count) new captures completed; expected $Frames." }
$finalState = Invoke-DB 'inspect' '{"kind":"state"}'
$finalCamera = Invoke-DB 'camera' '{"action":"get"}'
if ($finalState.pid -ne $initialState.pid -or $finalState.frame -le $initialState.frame) {
    throw 'Game process changed or frame counter stopped during capture.'
}
if ($finalCamera.pov -ne $initialCamera.pov -or
    [Math]::Abs($finalCamera.camX-$initialCamera.camX) -gt 1 -or
    [Math]::Abs($finalCamera.camY-$initialCamera.camY) -gt 1 -or
    [Math]::Abs($finalCamera.camYaw-$initialCamera.camYaw) -gt 0.01) {
    throw 'Camera moved; parked-camera flicker result is invalid.'
}
# Newest-first from the tool; only genuinely new captures, oldest first.
[array]::Reverse($fresh)
$rows = @()
$index = 0
foreach ($shot in $fresh) {
    $index++
    $copy = Join-Path $OutDir ('{0}-{1}-{2:d2}.png' -f (Get-Date -Format 'HHmmss'), $Label, $index)
    Copy-Item -LiteralPath $shot.path -Destination $copy -Force
    $captured.Add($copy)
    $rows += [pscustomobject]@{ frame = $index; luminance = (Get-MeanLuminance $copy); bytes = $shot.bytes; file = (Split-Path $copy -Leaf) }
}
if (!$KeepFrames) {
    # Keep the extremes for eyeballing, drop the rest.
    $ordered = @($rows | Sort-Object luminance)
    $keep = @($ordered[0].file, $ordered[-1].file)
    foreach ($row in $rows) {
        if ($keep -notcontains $row.file) { Remove-Item -LiteralPath (Join-Path $OutDir $row.file) -Force }
    }
}
$values = @($rows | ForEach-Object { $_.luminance })
$mean = ($values | Measure-Object -Average).Average
$min = ($values | Measure-Object -Minimum).Minimum
$max = ($values | Measure-Object -Maximum).Maximum
# Largest jump between neighbours is what actually reads as flicker; a slow
# drift across the run does not.
$biggestStep = 0.0
for ($i = 1; $i -lt $values.Count; $i++) {
    $step = [Math]::Abs($values[$i] - $values[$i - 1])
    if ($step -gt $biggestStep) { $biggestStep = $step }
}
[ordered]@{
    label = $Label
    frames = $rows.Count
    luminance = $values
    meanLuminance = [Math]::Round($mean, 2)
    minLuminance = $min
    maxLuminance = $max
    swing = [Math]::Round($max - $min, 2)
    largestStepBetweenFrames = [Math]::Round($biggestStep, 2)
    keptFrames = @($captured | Where-Object { Test-Path -LiteralPath $_ } | ForEach-Object { $_ })
} | ConvertTo-Json -Compress -Depth 4
