param(
    [ValidateSet('Scene', 'Depth', 'Triangle')][string]$Mode = 'Scene',
    [switch]$LoadRiverwood,
    [switch]$KeepVanillaWorld,
    # One-way diagnostic mode for raster reference captures; restart afterwards.
    [switch]$NativeReference,
    [int]$TimeoutSeconds = 60
)
$ErrorActionPreference = 'Stop'
$endpoint = 'http://127.0.0.1:8920/api/tool/'
function Invoke-DevBench([string]$Name, [hashtable]$Body) {
    Invoke-RestMethod -Uri ($endpoint + $Name) -Method Post -ContentType 'application/json' -Body ($Body | ConvertTo-Json -Compress) -TimeoutSec 8
}
# Run only against a Skyrim process deliberately launched with CS_REMIX_TEST=1.
# This script neither launches nor deploys files and never edits saved settings.
$deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
do {
    $menu = $null
    try { $menu = Invoke-DevBench 'menu' @{action='list'} } catch { }
    if ($menu -and ($menu.openMenus -contains 'Main Menu' -or $menu.openMenus -contains 'HUD Menu')) { break }
    Start-Sleep -Milliseconds 500
} while ([DateTime]::UtcNow -lt $deadline)
if (!$menu -or !($menu.openMenus -contains 'Main Menu' -or $menu.openMenus -contains 'HUD Menu')) {
    throw 'Timed out waiting for Skyrim main menu or HUD through DevBench.'
}
if ($NativeReference -and $menu.openMenus -notcontains 'Main Menu') {
    throw 'Native reference must be selected at the main menu before loading a world. Restart the game first.'
}
$settings = [ordered]@{
    'cs.nativeReference' = $NativeReference.ToString().ToLowerInvariant()
    'cs.scene' = ($Mode -eq 'Scene').ToString().ToLowerInvariant()
    'cs.depth' = ($Mode -eq 'Depth').ToString().ToLowerInvariant()
    'cs.suppressWorld' = ($Mode -eq 'Scene' -and !$KeepVanillaWorld).ToString().ToLowerInvariant()
    'rtx.zUp' = 'True'
    'rtx.debugView.debugViewIdx' = '0'
    'rtx.debugView.gpuPrint.enable' = 'False'
    # Remix's own default is already True. It is asserted rather than left alone
    # because dropping it from this list once turned frame generation off for a
    # whole session, which halves the presented rate and reads as the renderer
    # having got slower.
    'rtx.dlfg.enable' = 'True'
    # Nothing here overrides a Remix graphics setting. zUp is a coordinate
    # convention this game needs, and the two debug-view entries are already the
    # shipped defaults -- they are set only so a previous session's ImGui
    # fiddling cannot leak into a measurement.
    #
    # The preset is left at its default of Auto, so Remix picks for the GPU it
    # finds, and RTXDI, ray reconstruction, the NEE cache on the first bounce,
    # Remix post-processing and the auto-exposure speed are all left alone.
    # Note that Remix's rtx.dlfg is not what drives frame generation here: the
    # plugin's own Upscaling feature selects the method, and it reports
    # "method=FSR-FG fg=off" in [Perf] when it has not engaged one.
    # Medium used to be forced here for a third of the path-tracing time; the
    # goal is the scene Remix renders by default, so that trade is not ours to
    # make in the harness. Measurements taken under the old overrides are not
    # comparable with ones taken now.
    #
    # The measured condition is 1080p output with DLSS and frame generation at
    # whatever the auto preset selects, so the presented rate is roughly twice
    # the rendered one.
    #
    # The DLSS profile is NOT set here. Setting it at runtime resizes the
    # ray-tracing targets, and doing that while the frame-generation presenter is
    # live kills the process -- its present thread holds the motion-vector and
    # depth views the resize destroys. Pass -DlssProfile to LaunchTest.ps1
    # instead, which puts it in the environment before anything presents.
}
foreach ($setting in $settings.GetEnumerator()) {
    do {
        $result = Invoke-DevBench 'communityshaders.remix' @{ name = $setting.Key; value = $setting.Value }
        if ($result.success) { break }
        # The main-menu object can appear before its first rendered UI frame,
        # which is where the bridge registers the Vulkan device with Remix.
        Start-Sleep -Milliseconds 500
    } while ([DateTime]::UtcNow -lt $deadline)
    if (!$result.success) { throw "Remix setting rejected: $($setting.Key)" }
}
if ($LoadRiverwood) {
    $null = Invoke-DevBench 'console' @{command='coc riverwood'}
    # Entering an exterior on a fresh game raises the Survival Mode prompt. It is
    # modal, so the world behind it renders through the game's own path and every
    # capture taken while it is up is of vanilla Skyrim rather than of Remix.
    # DevBench 1.17 accepts a ZERO-based button index. Describe and answer the
    # observed Survival prompt, without moving the mouse/free camera. Never
    # dismiss an unrelated modal based on a guessed button or screen coordinate.
    $promptDeadline = [DateTime]::UtcNow.AddSeconds(240)
    $sawPrompt = $false
    $quiet = 0
    do {
        Start-Sleep -Seconds 3
        $state = $null
        try { $state = Invoke-DevBench 'menu' @{action='list'} } catch { }
        if (!$state) { continue }
        if ($state.messageBoxOpen) {
            $sawPrompt = $true
            $quiet = 0
            $modal = Invoke-DevBench 'menu' @{action='describe'}
            if ($modal.messageBoxOpen) {
                if ($modal.bodyText -notlike '*Enable Survival Mode?*') {
                    throw "Unexpected modal during Riverwood setup: $($modal.bodyText)"
                }
                $decline = [Array]::IndexOf([string[]]$modal.buttons, 'No')
                if ($decline -lt 0) { throw 'Survival prompt has no No button.' }
                $null = Invoke-DevBench 'menu' @{action='accept';index=$decline}
            }
            continue
        }
        # Skyrim raises a second prompt after the first is answered, so the loop
        # only stops once the box has stayed shut across several checks.
        if ($state.openMenus -notcontains 'Loading Menu') {
            $quiet++
            if ($quiet -ge 5) { break }
        } else {
            $quiet = 0
        }
    } while ([DateTime]::UtcNow -lt $promptDeadline)
    if ($quiet -lt 5) { throw 'Riverwood did not reach a stable non-modal state.' }
    if ($sawPrompt) { Write-Output 'Dismissed the Survival Mode prompt.' }
}
Write-Output "Configured $Mode test; native reference: $NativeReference; vanilla world bypass requested: $($settings['cs.suppressWorld'])."
