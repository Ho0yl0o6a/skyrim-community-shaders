param(
    [Parameter(Mandatory=$true)][ValidateSet('Native','Remix','Pair')][string]$Mode,
    [Parameter(Mandatory=$true)][ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$Label,
    [ValidateRange(0,1000)][int]$DebugView = 0,
    [string]$GameRoot = 'I:/SteamLibrary/steamapps/common/Skyrim Special Edition'
)
$ErrorActionPreference = 'Stop'
if ($Mode -eq 'Native' -and $DebugView) { throw 'Remix debug view is unavailable in native captures.' }
function Invoke-BufferTool([string]$Tool, [hashtable]$Body) {
    Invoke-RestMethod "http://127.0.0.1:8920/api/tool/$Tool" -Method Post -ContentType application/json -Body ($Body | ConvertTo-Json -Compress) -TimeoutSec 10
}
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$state = Invoke-BufferTool 'inspect' @{kind='state'}
if (!$state.playerLoaded) { throw 'Loaded world required.' }
$camera = Invoke-BufferTool 'camera' @{action='get'}
$request = [uint32]([DateTimeOffset]::UtcNow.ToUnixTimeSeconds())
$root = Join-Path $GameRoot 'Screenshots'
$metadataDirectory = Join-Path $root 'RemixBuffers'
$prefix = if ($Mode -eq 'Native') { 'native' } else { 'rtx' }
$metadataPattern = "$prefix-$($state.pid)-$request-*.json"
$before = @(Get-ChildItem -LiteralPath $root -Filter '*.dds' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName)
$output = Join-Path $workspace ".research/buffers/$Label-$request"
$null = New-Item -ItemType Directory -Path $output
try {
    if ($Mode -ne 'Native') {
        $viewResult = Invoke-BufferTool 'communityshaders.remix' @{name='rtx.debugView.debugViewIdx';value="$DebugView"}
        if (!$viewResult.success) { throw 'Runtime debug view rejected.' }
        $result = Invoke-BufferTool 'communityshaders.remix' @{name='rtx.captureDebugImage';value='true'}
        if (!$result.success) { throw 'Raw runtime capture option rejected.' }
        Start-Sleep -Milliseconds 500
    }
    $captureName = if ($Mode -eq 'Pair') { 'cs.captureBufferPair' } else { 'cs.captureBuffers' }
    $result = Invoke-BufferTool 'communityshaders.remix' @{name=$captureName;value="$request"}
    if (!$result.success) { throw 'Buffer capture request rejected. Pair mode requires LaunchTest -NativePreparation from startup.' }
    $deadline = [DateTime]::UtcNow.AddSeconds(45)
    do {
        Start-Sleep -Milliseconds 500
        $metadataFile = Get-ChildItem -LiteralPath $metadataDirectory -Filter $metadataPattern -ErrorAction SilentlyContinue | Select-Object -First 1
        $runtimeFiles = @(Get-ChildItem -LiteralPath $root -Filter '*.dds' | Where-Object { $_.FullName -notin $before })
        $complete = $metadataFile -and ($Mode -eq 'Native' -or
            (@($runtimeFiles | Where-Object Name -like 'gbufferAlbedo_*.dds').Count -gt 0 -and
             @($runtimeFiles | Where-Object Name -like 'gbufferLinearZ_*.dds').Count -gt 0))
    } while (!$complete -and [DateTime]::UtcNow -lt $deadline)
    if (!$complete) { throw 'Timed out waiting for buffer artifacts; inspect [Remix.buffers] log.' }
    # The exporter is asynchronous. Require a stable observed file set and
    # exclusive readability; a fixed delay alone can copy partially written DDS.
    if ($Mode -ne 'Native') {
        $lastSignature = ''
        $stablePolls = 0
        do {
            Start-Sleep -Milliseconds 500
            $runtimeFiles = @(Get-ChildItem -LiteralPath $root -Filter '*.dds' |
                Where-Object { $_.FullName -notin $before } | Sort-Object Name)
            $signature = ($runtimeFiles | ForEach-Object { "$($_.Name):$($_.Length):$($_.LastWriteTimeUtc.Ticks)" }) -join '|'
            $readable = $runtimeFiles.Count -gt 0
            foreach ($file in $runtimeFiles) {
                try {
                    $stream = [IO.File]::Open($file.FullName, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
                    try { if ($stream.Length -lt 128) { $readable = $false } } finally { $stream.Dispose() }
                } catch { $readable = $false }
            }
            if ($readable -and $signature -eq $lastSignature) { $stablePolls++ } else { $stablePolls = 0 }
            $lastSignature = $signature
        } while ($stablePolls -lt 3 -and [DateTime]::UtcNow -lt $deadline)
        if ($stablePolls -lt 3) { throw 'Runtime DDS files did not become stable/readable before timeout.' }
    }
    $metadata = Get-Content -LiteralPath $metadataFile.FullName -Raw | ConvertFrom-Json
    if ($metadata.nativeReference -ne ($Mode -eq 'Native')) { throw 'Capture mode mismatch.' }
    Copy-Item -LiteralPath $metadataFile.FullName -Destination $output
    if ($Mode -eq 'Native') {
        foreach ($buffer in $metadata.buffers.PSObject.Properties.Value) {
            if ($buffer.hresult -lt 0) { throw "Native buffer capture failed: $($buffer.hresult)" }
            Copy-Item -LiteralPath (Join-Path $GameRoot $buffer.path) -Destination $output
        }
    } else {
        if (!$metadata.queued) { throw 'Runtime capture was not queued.' }
        foreach ($file in $runtimeFiles) { Copy-Item -LiteralPath $file.FullName -Destination $output }
        $depthFiles = @($runtimeFiles | Where-Object Name -like 'gbufferLinearZ_*.dds')
        if ($depthFiles.Count -ne 1) { throw 'Expected one runtime depth capture for camera association.' }
        $cameraFile = $depthFiles[0].FullName + '.camera.json'
        $rayCamera = Get-Content -LiteralPath $cameraFile -Raw | ConvertFrom-Json
        if ($rayCamera.schema -ne 1 -or $rayCamera.source -ne 'uploaded-raytrace-args' -or
            $rayCamera.depthBuffer -ne $depthFiles[0].Name -or $null -eq $rayCamera.runtimeFrame) {
            throw 'Runtime ray-camera metadata is missing or inconsistent.'
        }
        Copy-Item -LiteralPath $cameraFile -Destination $output
    }
    if ($Mode -eq 'Pair') {
        if (!$metadata.pairedCapture -or !$metadata.pairedNativeCaptured -or !$metadata.nativePreparation) { throw 'Missing paired native capture/preparation acknowledgement.' }
        $nativeFile = @(Get-ChildItem -LiteralPath $metadataDirectory -Filter "native-$($state.pid)-$request-*.json")
        if ($nativeFile.Count -ne 1) { throw 'Expected exactly one native half of the paired capture.' }
        $native = Get-Content -LiteralPath $nativeFile[0].FullName -Raw | ConvertFrom-Json
        if (!$native.pairedCapture -or !$native.nativeReference -or !$native.nativePreparation -or $native.request -ne $metadata.request -or
            $native.pid -ne $metadata.pid -or $native.frame -ne $metadata.frame) { throw 'Paired capture frame/identity mismatch.' }
        Copy-Item -LiteralPath $nativeFile[0].FullName -Destination $output
        foreach ($buffer in $native.buffers.PSObject.Properties.Value) {
            if ($buffer.hresult -lt 0) { throw "Paired native buffer capture failed: $($buffer.hresult)" }
            Copy-Item -LiteralPath (Join-Path $GameRoot $buffer.path) -Destination $output
        }
    }
    $manifest = [ordered]@{mode=$Mode;debugView=$DebugView;request=$request;state=$state;cameraBefore=$camera;
        cameraAfter=(Invoke-BufferTool 'camera' @{action='get'});scope='Raw capture only, not a parity verdict.';
        artifacts=@(Get-ChildItem -LiteralPath $output | Select-Object Name,Length)}
    $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'manifest.json') -Encoding utf8
    [ordered]@{directory=$output;mode=$Mode;files=$manifest.artifacts.Count;camera=$manifest.cameraAfter} | ConvertTo-Json -Depth 4 -Compress
} finally {
    if ($Mode -ne 'Native') {
        foreach ($setting in @(@{name='rtx.captureDebugImage';value='false'}, @{name='rtx.debugView.debugViewIdx';value='0'})) {
            try { $null = Invoke-BufferTool 'communityshaders.remix' $setting }
            catch { Write-Warning "Could not restore $($setting.name): $($_.Exception.Message)" }
        }
    }
}
