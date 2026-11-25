<#
.SYNOPSIS
  Builds QUADmapper-Setup-<version>.exe (ADR-057), without the AI engine (TD-44).

.DESCRIPTION
  1. cmake --build the Release app (build/release must be configured: release-local on
     this PC, release elsewhere).
  2. cmake --install it into .scratch/installer/stage.
  3. ISCC packaging/QUADmapper.iss -> packaging/out/QUADmapper-Setup-<version>.exe.
  The version is the one in the top-level CMakeLists.txt project(VERSION).
  -Step Stage stops after step 2 and -Step Package runs only step 3, so CI can sign the
  staged QUADmapper.exe in between (.github/workflows/release.yml).
#>
[CmdletBinding()]
param(
    [string]$BuildDir = 'build/release',
    [ValidateSet('All', 'Stage', 'Package')]
    [string]$Step = 'All'
)
$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$buildPath = Join-Path $repo $BuildDir
$stage = Join-Path $repo '.scratch/installer/stage'
$out = Join-Path $PSScriptRoot 'out'

$cmakeLists = Get-Content -Raw (Join-Path $repo 'CMakeLists.txt')
if ($cmakeLists -notmatch 'project\s*\(\s*QUADmapper[^)]*VERSION\s+(\d+\.\d+\.\d+)') {
    throw 'No project(QUADmapper VERSION x.y.z) in CMakeLists.txt'
}
$version = $Matches[1]

$iscc = @(
    (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
    (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 not found (winget install JRSoftware.InnoSetup)' }

if ($Step -ne 'Package') {
    if (-not (Test-Path -LiteralPath (Join-Path $buildPath 'CMakeCache.txt'))) {
        throw "$BuildDir is not configured: cmake --preset release-local (or release)"
    }

    Write-Host "QUADmapper ${version}: building Release"
    cmake --build $buildPath --config Release --target QUADmapper
    if ($LASTEXITCODE -ne 0) { throw 'Release build failed' }

    Write-Host "Staging into $stage"
    if (Test-Path -LiteralPath $stage) { Remove-Item -Recurse -Force -LiteralPath $stage }
    cmake --install $buildPath --config Release --prefix $stage
    if ($LASTEXITCODE -ne 0) { throw 'cmake --install failed' }
}
if (-not (Test-Path -LiteralPath (Join-Path $stage 'QUADmapper.exe'))) { throw 'Staged folder has no QUADmapper.exe' }
if ($Step -eq 'Stage') {
    Write-Host "Staged: $stage"
    return
}

Write-Host 'Building the installer'
& $iscc "/DAppVersion=$version" "/DStageDir=$stage" "/DOutputDir=$out" (Join-Path $PSScriptRoot 'QUADmapper.iss')
if ($LASTEXITCODE -ne 0) { throw 'ISCC failed' }

$setup = Join-Path $out "QUADmapper-Setup-$version.exe"
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $setup).Hash
Write-Host "Done: $setup ($([math]::Round((Get-Item $setup).Length / 1MB, 1)) MB)"
Write-Host "SHA-256: $hash"
