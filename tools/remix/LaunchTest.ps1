param(
    [string]$GameRoot = 'I:/SteamLibrary/steamapps/common/Skyrim Special Edition',
    [string]$LogDir = (Join-Path $PSScriptRoot '../../.research/testlogs'),
    [switch]$NoGpuTiming,
    [switch]$ApiTiming,
    # Diagnostic only: shader debug records and GPU allocation lifetime tracking.
    [switch]$GpuCrashDiagnostics,
    [switch]$GrassProbe,
    [switch]$SkinProbe,
    # Bounded GPU validation of the 898-vertex, two-bone MaleHeadNord class.
    [switch]$FaceSkinProbe,
    # Alternate facial class; samples only while paired debug view 823 is active.
    [ValidateRange(1, 4096)][int]$SkinProbeVertices = 898,
    # Startup-only isolation; never resize reconstruction targets in a live run.
    [switch]$NoRayReconstruction,
    # Diagnostic only: bypass both upscaling and denoising at startup.
    [switch]$RawLighting,
    # Isolate denoising at native resolution; does not disable the denoiser.
    [switch]$NoUpscaling,
    # Diagnostic: keep native CPU batch preparation/cleanup, not native GPU draws.
    [switch]$NativePreparation,
    # Paired-buffer diagnostic only: full resolution, no temporal sample jitter.
    [switch]$MatchCaptureSamples,
    # Per-frame runtime ownership evidence; adds CPU/logging overhead.
    [switch]$RetainedAudit,
    # Reproduce the pre-fix behaviour: submit whatever camera and rebasing origin
    # the shadow state happens to hold, instead of holding the last world pair.
    # For A/B-ing the flicker fix against the same binary.
    [switch]$NoCameraHold,
    # DLSS profile, applied at startup rather than through the config API.
    # Changing it at runtime resizes the ray-tracing targets, and doing that
    # while the frame-generation presenter is live kills the process: its
    # present thread holds the motion-vector and depth views the resize
    # destroys. Setting it here means the profile is already in place before
    # anything presents. 0 UltraPerf, 1 MaxPerf, 2 Balanced, 3 MaxQuality,
    # 5 FullResolution (native). Empty leaves Remix on Auto.
    [string]$DlssProfile = '',
    # Refit the top-level acceleration structure instead of rebuilding it every
    # frame. Off by default until it is proven; a wrong top-level structure
    # renders corrupt geometry rather than failing loudly.
    [switch]$TlasRefit,
    # Skip the Reflex out-of-band present/render markers and the OOB queue
    # notification. The driver paces a present it has been told is a
    # frame-generation present with a timed sleep inside vkQueuePresentKHR of
    # about half a frame, per present, on the thread that drains the DLFG ring;
    # that sleep is what held the frame rate at ~53 presented. Without the
    # markers the same scene presents at 107 (Auto) / 149 (UltraPerformance).
    # The cost is Reflex's latency accounting around the DLFG presents, not any
    # rendering. rtx.reflexMode does not reach these markers.
    [switch]$NoOobMarkers,
    # Release a backbuffer ring slot once the blit that consumes it is submitted
    # rather than after both presents return. The blit already signals the
    # semaphore the next writer of that backbuffer waits on, so GPU ordering is
    # unchanged; this only lets the game thread run ahead of presenting.
    [switch]$EarlyBackbufferRelease,
    # Emit the Reflex SIMULATION_START/END markers and run the Reflex sleep on the
    # game thread inside Present (d3d11_swapchain.cpp, kD3D11ReflexSimulation).
    # Off by default in the runtime, which leaves the driver with render/present/
    # out-of-band markers but no simulation interval, so its frame-generation
    # pacing can only follow its own present cadence. Also the only way Reflex
    # low-latency mode does anything on this path: without it sleep() never runs.
    [switch]$ReflexSim
)
$ErrorActionPreference = 'Stop'
if ($SkinProbeVertices -ne 898) { $FaceSkinProbe = $true }
if ($MatchCaptureSamples) {
    if ($DlssProfile -and $DlssProfile -ne '5') { throw 'Matched capture samples require full-resolution DLSS profile 5.' }
    $NativePreparation = $true
    $DlssProfile = '5'
    Write-Output 'Diagnostic matched samples: native CPU preparation, full resolution, temporal jitter disabled.'
}
# Launches the already-deployed test build. It never deploys, never edits saved
# settings, and never deletes files. The opt-in environment variables are set on
# the child process only, so ordinary launches are unaffected.
if (Get-Process SkyrimSE -ErrorAction SilentlyContinue) { throw 'Skyrim is already running.' }
$GameRoot = (Resolve-Path -LiteralPath $GameRoot).Path
$loader = Join-Path $GameRoot 'skse64_loader.exe'
if (!(Test-Path -LiteralPath $loader -PathType Leaf)) { throw "Missing SKSE loader: $loader" }
$env:CS_REMIX_TEST = '1'
if ($GpuCrashDiagnostics) {
    $env:DXVK_ENABLE_AFTERMATH = '1'
    $env:DXVK_ENABLE_AFTERMATH_RESOURCE_TRACKING = '1'
} else {
    Remove-Item Env:DXVK_ENABLE_AFTERMATH -ErrorAction SilentlyContinue
    Remove-Item Env:DXVK_ENABLE_AFTERMATH_RESOURCE_TRACKING -ErrorAction SilentlyContinue
}
# The Remix runtime logs the phase timings this harness reads. Its default home
# is the SKSE log folder on the system drive; when that volume is short of space
# the writes stop and the measurement loses its evidence, so tests keep the log
# beside the workspace. The plugin honours a DXVK_LOG_PATH that is already set.
$LogDir = [IO.Path]::GetFullPath($LogDir)
$null = New-Item -ItemType Directory -Force -Path $LogDir
$env:DXVK_LOG_PATH = $LogDir
if (!$NoGpuTiming) { $env:CS_REMIX_GPU_TIMING = '1' } else { Remove-Item Env:CS_REMIX_GPU_TIMING -ErrorAction SilentlyContinue }
# Timing every deferred Remix API command costs about 0.8 ms of the frame it
# measures, so it is off unless a run is specifically accounting for the
# command-stream thread.
if ($ApiTiming) { $env:CS_REMIX_API_TIMING = '1' } else { Remove-Item Env:CS_REMIX_API_TIMING -ErrorAction SilentlyContinue }
if ($GrassProbe) { $env:CS_REMIX_GRASS_PROBE = '1' } else { Remove-Item Env:CS_REMIX_GRASS_PROBE -ErrorAction SilentlyContinue }
if ($SkinProbe -or $FaceSkinProbe) { $env:CS_REMIX_SKIN_PROBE = '1' } else { Remove-Item Env:CS_REMIX_SKIN_PROBE -ErrorAction SilentlyContinue }
if ($FaceSkinProbe) { $env:CS_REMIX_FACE_SKIN_PROBE = '1' } else { Remove-Item Env:CS_REMIX_FACE_SKIN_PROBE -ErrorAction SilentlyContinue }
if ($FaceSkinProbe) { $env:CS_REMIX_SKIN_PROBE_VERTICES = "$SkinProbeVertices" } else { Remove-Item Env:CS_REMIX_SKIN_PROBE_VERTICES -ErrorAction SilentlyContinue }
if ($NoRayReconstruction -or $RawLighting -or $NoUpscaling) { $env:DXVK_RAY_RECONSTRUCTION = '0' } else { Remove-Item Env:DXVK_RAY_RECONSTRUCTION -ErrorAction SilentlyContinue }
if ($RawLighting -or $NoUpscaling) {
    $env:DXVK_UPSCALER_TYPE = '0'
} else {
    Remove-Item Env:DXVK_UPSCALER_TYPE -ErrorAction SilentlyContinue
}
if ($RawLighting) { $env:DXVK_USE_DENOISER = '0' } else { Remove-Item Env:DXVK_USE_DENOISER -ErrorAction SilentlyContinue }
if ($NativePreparation) { $env:CS_REMIX_KEEP_NATIVE_PREPARATION = '1' } else { Remove-Item Env:CS_REMIX_KEEP_NATIVE_PREPARATION -ErrorAction SilentlyContinue }
if ($MatchCaptureSamples) { $env:CS_REMIX_MATCH_CAPTURE_SAMPLES = '1' } else { Remove-Item Env:CS_REMIX_MATCH_CAPTURE_SAMPLES -ErrorAction SilentlyContinue }
if ($RetainedAudit) { $env:CS_REMIX_AUDIT_RETAINED = '1' } else { Remove-Item Env:CS_REMIX_AUDIT_RETAINED -ErrorAction SilentlyContinue }
if ($NoCameraHold) { $env:CS_REMIX_CAMERA_HOLD = '0' } else { Remove-Item Env:CS_REMIX_CAMERA_HOLD -ErrorAction SilentlyContinue }
if ($DlssProfile) { $env:RTX_QUALITY_DLSS_OVERRIDE = $DlssProfile } else { Remove-Item Env:RTX_QUALITY_DLSS_OVERRIDE -ErrorAction SilentlyContinue }
if ($TlasRefit) { $env:CS_REMIX_TLAS_REFIT = '1' } else { Remove-Item Env:CS_REMIX_TLAS_REFIT -ErrorAction SilentlyContinue }
if ($NoOobMarkers) { $env:CS_REMIX_NO_OOB_MARKERS = '1' } else { Remove-Item Env:CS_REMIX_NO_OOB_MARKERS -ErrorAction SilentlyContinue }
if ($EarlyBackbufferRelease) { $env:CS_REMIX_EARLY_BB_RELEASE = '1' } else { Remove-Item Env:CS_REMIX_EARLY_BB_RELEASE -ErrorAction SilentlyContinue }
if ($ReflexSim) { $env:CS_REMIX_D3D11_REFLEX_SIM = '1' } else { Remove-Item Env:CS_REMIX_D3D11_REFLEX_SIM -ErrorAction SilentlyContinue }
$process = Start-Process -FilePath $loader -WorkingDirectory $GameRoot -PassThru
Write-Output "Launched SKSE loader pid $($process.Id) from $GameRoot."
$deadline = [DateTime]::UtcNow.AddSeconds(120)
do {
    Start-Sleep -Seconds 2
    $game = Get-Process SkyrimSE -ErrorAction SilentlyContinue
} while (!$game -and [DateTime]::UtcNow -lt $deadline)
if (!$game) { throw 'SkyrimSE did not start.' }
Write-Output "SkyrimSE pid $($game.Id) started $($game.StartTime.ToString('HH:mm:ss'))."
