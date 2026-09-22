<#
.SYNOPSIS
    Build the DXVK d3d11/dxgi DLLs with profile-guided optimisation.

.DESCRIPTION
    Worth several percent on top of the LTCG build. Kept out of tools/build-dxvk.ps1 because it
    needs a real game run in the middle, so it belongs in a release step, not an incremental one.

        tools\build-dxvk-pgo.ps1 -Step Instrument
        # launch the game and play the scenes you want represented, then WHILE IT IS RUNNING:
        tools\build-dxvk-pgo.ps1 -Step Sweep
        tools\build-dxvk-pgo.ps1 -Step Optimize

    Sweep is a separate step because the profile is only written at a clean process exit, so a
    harness that kills the game produces no .pgc at all. pgosweep dumps the counters out of the
    live process instead.

    Two things this works around:

    * /USEPROFILE cannot go in cpp_link_args. Meson re-runs its find_library probes on a
      reconfigure, a probe link with no matching .pgd fails, and meson reports that as a missing
      d3d9 library. It goes in link.exe's LINK environment variable instead, which only affects
      the real link. Optimize therefore builds once cleanly so build.ninja is current, then
      relinks just the two DLLs -- the second compile must not regenerate, or the probes re-run.

    * The instrumented DLLs need pgort140.dll beside them, staged by Instrument and removed
      again by Optimize.

.PARAMETER Step
    Instrument, Sweep, or Optimize. See above.

.PARAMETER DeployDir
    Where the game loads dxvk_d3d11.dll from. The instrumented DLLs and pgort140.dll are copied
    here, and the optimised ones replace them. Defaults to the CommunityShadersOutputDir bin
    directory when that is set.

.PARAMETER BuildDir
    Meson build directory, kept separate from the normal build so an instrumented binary can
    never be mistaken for a shipping one. Defaults to extern/dxvk/build-pgo.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Instrument', 'Sweep', 'Optimize')]
    [string]$Step,

    [string]$DeployDir,
    [string]$BuildDir
)

$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Split-Path -Parent $ScriptDir
$DxvkSrc   = Join-Path $RepoRoot 'extern\dxvk'
if (-not $BuildDir) { $BuildDir = Join-Path $DxvkSrc 'build-pgo' }

if (-not (Test-Path (Join-Path $DxvkSrc 'meson.build'))) {
    Write-Error "[pgo] extern/dxvk is not checked out at $DxvkSrc"
    exit 1
}

# meson --vsenv shells out to vswhere, which is not on PATH in a plain shell.
$vswhereDir = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer'
if (Test-Path (Join-Path $vswhereDir 'vswhere.exe')) {
    $env:Path = "$vswhereDir;$env:Path"
}

$meson = (Get-Command meson -ErrorAction SilentlyContinue).Source
if (-not $meson) {
    Write-Error "[pgo] meson is required; install it with 'python -m pip install meson ninja'"
    exit 1
}

# Resolve the toolset rather than hardcoding a version: the PGO runtime and pgosweep move with
# every VS update, and a stale path fails late and confusingly.
function Get-MsvcToolDir {
    $vs = & (Join-Path $vswhereDir 'vswhere.exe') -latest -property installationPath 2>$null
    if (-not $vs) { return $null }
    $verFile = Join-Path $vs 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt'
    if (-not (Test-Path $verFile)) { return $null }
    $ver = (Get-Content $verFile -Raw).Trim()
    $dir = Join-Path $vs "VC\Tools\MSVC\$ver\bin\Hostx64\x64"
    if (Test-Path $dir) { return $dir }
    return $null
}

$toolDir = Get-MsvcToolDir
if (-not $toolDir) {
    Write-Error "[pgo] could not locate the MSVC x64 tool directory via vswhere"
    exit 1
}

if (-not $DeployDir) {
    $out = $env:CommunityShadersOutputDir
    if ($out) {
        $first = ($out -split ';')[0].Trim()
        if ($first) { $DeployDir = Join-Path $first 'SKSE\Plugins\CommunityShaders\bin' }
    }
}

$d3d11Dll = Join-Path $BuildDir 'src\d3d11\dxvk_d3d11.dll'
$dxgiDll  = Join-Path $BuildDir 'src\dxgi\dxvk_dxgi.dll'

# A running game holds the DLLs open, so the deploy fails after a full instrumented link has
# already been paid for. Say so up front instead.
function Assert-DeployWritable {
    if (-not $DeployDir) { return }
    $target = Join-Path $DeployDir 'dxvk_d3d11.dll'
    if (-not (Test-Path $target)) { return }
    try {
        $fs = [System.IO.File]::Open($target, 'Open', 'Write', 'None')
        $fs.Close()
    } catch {
        Write-Error "[pgo] $target is in use -- close the game before running this step"
        exit 1
    }
}

function Invoke-MesonSetup {
    if (Test-Path (Join-Path $BuildDir 'build.ninja')) { return }
    Write-Host "[pgo] configuring $BuildDir"
    # Same flags as tools/build-dxvk.ps1, so the only difference between this build and the
    # shipping one is the profile.
    & $meson setup $BuildDir $DxvkSrc --vsenv --buildtype release `
        -Db_ndebug=true `
        -Dcpp_args='/arch:AVX /GL' -Dc_args='/GL' `
        -Dcpp_link_args='/LTCG' -Dc_link_args='/LTCG' `
        -Denable_d3d8=false -Denable_d3d9=false -Denable_d3d10=false 2>&1 | Write-Host
    if (-not (Test-Path (Join-Path $BuildDir 'build.ninja'))) {
        Write-Error "[pgo] meson setup failed"
        exit 1
    }
}

switch ($Step) {
    'Instrument' {
        Assert-DeployWritable
        Invoke-MesonSetup
        Write-Host "[pgo] relinking with /GENPROFILE"
        & $meson configure $BuildDir -Dcpp_link_args='/LTCG /GENPROFILE' -Dc_link_args='/LTCG /GENPROFILE' | Out-Null
        & $meson compile -C $BuildDir 2>&1 | Select-String -Pattern 'error|FAILED|Linking' | Select-Object -Last 8
        if (-not (Test-Path $d3d11Dll)) {
            Write-Error "[pgo] instrumented build produced no DLL"
            exit 1
        }
        if ($DeployDir) {
            Copy-Item $d3d11Dll $DeployDir -Force
            Copy-Item $dxgiDll  $DeployDir -Force
            # The instrumented DLLs will not load without this next to them.
            Copy-Item (Join-Path $toolDir 'pgort140.dll') $DeployDir -Force
            Write-Host "[pgo] instrumented DLLs + pgort140.dll staged into $DeployDir"
        } else {
            Write-Host "[pgo] instrumented DLLs at $BuildDir (no DeployDir; copy them yourself, with pgort140.dll)"
        }
        Write-Host "[pgo] now run the game through the scenes you want represented, then -Step Sweep while it is still running"
    }

    'Sweep' {
        $proc = Get-Process SkyrimSE -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $proc) {
            Write-Error "[pgo] SkyrimSE is not running; the profile is only written at a clean exit, so sweep it live"
            exit 1
        }
        $sweep = Join-Path $toolDir 'pgosweep.exe'
        $pgd = Join-Path $BuildDir 'src\d3d11\dxvk_d3d11!1.pgc'
        $target = if ($DeployDir) { Join-Path $DeployDir 'dxvk_d3d11.dll' } else { $d3d11Dll }
        & $sweep /pid $proc.Id $target $pgd 2>&1 | ForEach-Object { Write-Host "[pgo]   $_" }
        if (Test-Path $pgd) {
            Write-Host "[pgo] wrote $pgd ($((Get-Item $pgd).Length) bytes)"
        } else {
            Write-Error "[pgo] pgosweep produced no .pgc"
            exit 1
        }
    }

    'Optimize' {
        Assert-DeployWritable
        $pgc = @(Get-ChildItem $BuildDir -Recurse -Filter '*.pgc' -ErrorAction SilentlyContinue)
        if (-not $pgc) {
            Write-Error "[pgo] no .pgc files under $BuildDir; run -Step Instrument, play, then -Step Sweep"
            exit 1
        }
        Write-Host "[pgo] using $($pgc.Count) profile file(s), $([int](($pgc | Measure-Object Length -Sum).Sum / 1KB)) KB total"

        # Configure and build once with a clean environment so build.ninja is current and the
        # find_library probes pass without /USEPROFILE in the link args.
        Remove-Item Env:LINK -ErrorAction SilentlyContinue
        & $meson configure $BuildDir -Dcpp_link_args='/LTCG' -Dc_link_args='/LTCG' | Out-Null
        & $meson compile -C $BuildDir 2>&1 | Select-String -Pattern 'ERROR|FAILED' | Select-Object -Last 4
        if (-not (Test-Path $d3d11Dll)) {
            Write-Error "[pgo] clean relink failed"
            exit 1
        }

        # Relink only the two DLLs with the profile applied. Deleting them first is what makes
        # ninja relink without regenerating, which would re-run the probes.
        Remove-Item $d3d11Dll, $dxgiDll -ErrorAction SilentlyContinue
        $env:LINK = '/USEPROFILE'
        try {
            & $meson compile -C $BuildDir 2>&1 | Select-String -Pattern 'error|FAILED|Linking|PGU|warning LNK' | Select-Object -Last 12
        } finally {
            Remove-Item Env:LINK -ErrorAction SilentlyContinue
        }
        if (-not (Test-Path $d3d11Dll)) {
            Write-Error "[pgo] optimised relink failed"
            exit 1
        }
        Write-Host "[pgo] optimised dxvk_d3d11.dll = $((Get-Item $d3d11Dll).Length) bytes"

        if ($DeployDir) {
            Copy-Item $d3d11Dll $DeployDir -Force
            Copy-Item $dxgiDll  $DeployDir -Force
            # Only the instrumented build needs the PGO runtime.
            Remove-Item (Join-Path $DeployDir 'pgort140.dll') -Force -ErrorAction SilentlyContinue
            Write-Host "[pgo] deployed to $DeployDir"
        }
    }
}
