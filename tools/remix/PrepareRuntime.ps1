param([Parameter(Mandatory=$true)][string]$Source)
$ErrorActionPreference = 'Stop'
$Source = (Resolve-Path -LiteralPath $Source).Path
$expected = '62378bcf'
$head = git -C $Source rev-parse --short=8 HEAD
if ($LASTEXITCODE -ne 0 -or $head -ne $expected) { throw "Expected Frissj/Titanfall-2-Remix commit $expected, found $head" }
$changes = git -C $Source status --porcelain --untracked-files=no --ignore-submodules
if ($changes) { throw 'Use a clean, dedicated runtime clone. This script replaces its D3D11 capture host.' }
$patch = Join-Path $PSScriptRoot 'runtime.patch'
git -C $Source apply --check $patch
if ($LASTEXITCODE -ne 0) { throw 'Runtime patch validation failed' }
git -C $Source apply $patch
if ($LASTEXITCODE -ne 0) { throw 'Runtime patch application failed' }
foreach ($name in @('d3d11_rtx.cpp', 'd3d11_rtx.h')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "runtime/$name") -Destination (Join-Path $Source "src/d3d11/$name") -Force
}
Write-Output 'Applied the Skyrim API-only D3D11/Vulkan host. Build with the flags in docs/development/remix-vulkan.md.'
