param(
    [string]$Source = (Join-Path $PSScriptRoot '../../.research/dxvk-remix'),
    [string]$DeveloperSetup = 'I:\Microsoft Visual Studio\Product\VC\Auxiliary\Build\vcvarsall.bat',
    [ValidateRange(1, 64)][int]$Jobs = 8
)
$ErrorActionPreference = 'Stop'
$Source = (Resolve-Path -LiteralPath $Source).Path
$DeveloperSetup = (Resolve-Path -LiteralPath $DeveloperSetup).Path
if ($DeveloperSetup.Contains('"')) { throw 'Unsupported quote in developer setup path.' }
$env:PM_PACKAGES_ROOT = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../.research/packman')).Replace('\', '/')
# The linker writes a response-file temporary and fails the whole link with
# LNK1108 ("cannot write file at 0x0") when the drive holding TEMP is full.
# This machine's system drive is, so keep the build's temporaries beside the
# build rather than depending on free space there. Process-local: nothing
# outside this script sees the change.
$buildTemp = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../.research/buildtemp'))
$null = New-Item -ItemType Directory -Force -Path $buildTemp
$env:TMP = $buildTemp
$env:TEMP = $buildTemp
# vcvarsall resolves the toolchain through vswhere, which lives with the VS
# installer rather than the relocated product directory.
$installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
if ((Test-Path -LiteralPath (Join-Path $installer 'vswhere.exe')) -and ($env:PATH -notlike "*$installer*")) {
    $env:PATH = "$installer;$env:PATH"
}
Push-Location -LiteralPath $Source
try {
    # This fork emits shader headers as side effects of _built_shaders.txt.
    # Ninja scans C++ dependencies before generation in a combined invocation,
    # leaving some embedded shaders one build behind. Finish header generation
    # first, then start a fresh dependency scan for the DLL build.
    $command = '"{0}" x64 && meson compile -C _Comp64Release -j {1} rtx_shaders && meson compile -C _Comp64Release -j {1}' -f $DeveloperSetup, $Jobs
    & cmd /d /s /c $command
    if ($LASTEXITCODE -ne 0) { throw "Runtime build failed: $LASTEXITCODE" }
} finally {
    Pop-Location
}
