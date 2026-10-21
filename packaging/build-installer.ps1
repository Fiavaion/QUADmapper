<#
.SYNOPSIS
  Stages QUADmapper and its bundled AI runtime, then builds the Inno Setup installer (ADR-057).

.DESCRIPTION
  1. App: QUADmapper.exe from the Release build, the DLLs it loads (checked with
     dumpbin /dependents), the MSVC runtime (app-local), and resources/.
  2. AI: the python.org embeddable Python 3.11 (SHA-256 pinned), requirements.lock
     installed into it with --require-hashes --no-deps --target, sam2 from
     requirements-sam2.lock built against that torch, the sidecar .py files (no tests),
     and the models from %LOCALAPPDATA%\QUADmapper\models checked against the SHA-256
     pins in tools/ai-sidecar/setup.ps1.
  3. THIRD_PARTY_NOTICES.txt, gathered from the installed packages and vcpkg.
  4. ISCC packaging/QUADmapper.iss -> packaging/out/QUADmapper-Setup-<version>.exe.

  Staging goes to packaging/stage (git-ignored). The Python runtime is re-installed only
  when the embeddable zip or a lock file changes (stamp file); everything else is
  re-copied on every run. The repo's tools/ai-sidecar/.venv is never touched.

.PARAMETER BuildDir
  Folder holding the Release QUADmapper.exe. Default: build/release/src/app/Release.

.PARAMETER ModelsDir
  Source of the model files. Default: $env:QM_MODELS_DIR, else %LOCALAPPDATA%\QUADmapper\models.

.PARAMETER StageOnly
  Stage everything but do not run ISCC.
#>
[CmdletBinding()]
param(
    [string]$BuildDir,
    [string]$ModelsDir,
    [switch]$StageOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Repo = Split-Path -Parent $PSScriptRoot
$Packaging = $PSScriptRoot
$Stage = Join-Path $Packaging 'stage'
$StageApp = Join-Path $Stage 'app'
$StageAi = Join-Path $Stage 'ai'
$Out = Join-Path $Packaging 'out'
$SidecarSrc = Join-Path $Repo 'tools\ai-sidecar'

if (-not $BuildDir) { $BuildDir = Join-Path $Repo 'build\release\src\app\Release' }
if (-not $ModelsDir) {
    if ($env:QM_MODELS_DIR) { $ModelsDir = $env:QM_MODELS_DIR }
    else { $ModelsDir = Join-Path $env:LOCALAPPDATA 'QUADmapper\models' }
}

# python.org embeddable package, Windows amd64. The MD5 python.org publishes for it
# (6d9aa08531d48fcc261ba667e2df17c4) was checked when this SHA-256 was pinned.
$PythonVersion = '3.11.9'
$PythonZipUrl = "https://www.python.org/ftp/python/$PythonVersion/python-$PythonVersion-embed-amd64.zip"
$PythonZipSha256 = '009d6bf7e3b2ddca3d784fa09f90fe54336d5b60f0e0f305c37f400bf83cfd3b'
$PipVersion = '25.2'

function Invoke-Checked {
    param([string]$What, [scriptblock]$Command)
    # Windows PowerShell 5.1 turns a native tool's stderr into a terminating error
    # under 'Stop'; judge native tools by their exit code instead.
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $Command } finally { $ErrorActionPreference = $saved }
    if ($LASTEXITCODE -ne 0) { throw "$What failed (exit code $LASTEXITCODE)" }
}

function Get-Sha256([string]$File) {
    (Get-FileHash -Algorithm SHA256 -LiteralPath $File).Hash.ToLowerInvariant()
}

function Reset-Dir([string]$Path) {
    if (Test-Path -LiteralPath $Path) { Remove-Item -LiteralPath $Path -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $Path | Out-Null
}

function Get-DirSizeMB([string]$Path) {
    $sum = (Get-ChildItem -LiteralPath $Path -Recurse -File | Measure-Object -Property Length -Sum).Sum
    [math]::Round($sum / 1MB)
}

# --- version -------------------------------------------------------------------
$cmake = Get-Content -Raw -LiteralPath (Join-Path $Repo 'CMakeLists.txt')
$m = [regex]::Match($cmake, 'project\s*\(\s*QUADmapper\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)')
if (-not $m.Success) { throw 'Could not read the project VERSION from CMakeLists.txt' }
$Version = $m.Groups[1].Value
Write-Host "QUADmapper $Version"

# --- tools -----------------------------------------------------------------------
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'Visual Studio with the C++ tools was not found (vswhere)' }
$dumpbin = Get-ChildItem (Join-Path $vsPath 'VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe') |
    Sort-Object FullName | Select-Object -Last 1
if (-not $dumpbin) { throw 'dumpbin.exe not found' }
$crtDir = Get-ChildItem (Join-Path $vsPath 'VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT') -Directory |
    Sort-Object FullName | Select-Object -Last 1
if (-not $crtDir) { throw 'The MSVC redistributable CRT folder was not found' }
$iscc = Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'
if (-not $StageOnly -and -not (Test-Path -LiteralPath $iscc)) { throw "Inno Setup 6 not found at $iscc" }

function Get-Dependents([string]$File) {
    $lines = & $dumpbin.FullName /nologo /dependents $File
    if ($LASTEXITCODE -ne 0) { throw "dumpbin failed on $File" }
    $lines | Where-Object { $_ -match '^\s{4}\S+\.dll\s*$' } | ForEach-Object { $_.Trim() }
}

# --- 1. app ----------------------------------------------------------------------
$exe = Join-Path $BuildDir 'QUADmapper.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "No Release build at $exe (cmake --build build/release --config Release)" }
Write-Host "app: $BuildDir"
Reset-Dir $StageApp
Copy-Item -LiteralPath $exe -Destination $StageApp

# The MSVC runtime goes app-local (Microsoft's redistributable files), so no VC++
# Redistributable install is needed.
$crtNames = @{}
Get-ChildItem -LiteralPath $crtDir.FullName -Filter *.dll | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $StageApp
    $crtNames[$_.Name.ToLowerInvariant()] = $true
}

# Walk the import tables from the exe: a DLL found beside the build output is
# shipped; anything else must be a Windows system DLL.
$system32 = Join-Path $env:windir 'System32'
$queue = New-Object System.Collections.Queue
$queue.Enqueue($exe)
$seen = @{}
while ($queue.Count -gt 0) {
    $file = $queue.Dequeue()
    foreach ($dep in (Get-Dependents $file)) {
        $key = $dep.ToLowerInvariant()
        if ($seen.ContainsKey($key)) { continue }
        $seen[$key] = $true
        $local = Join-Path $BuildDir $dep
        if (Test-Path -LiteralPath $local) {
            Copy-Item -LiteralPath $local -Destination $StageApp
            $queue.Enqueue($local)
        } elseif ($crtNames.ContainsKey($key)) {
            continue
        } elseif ($key -like 'api-ms-win-*' -or $key -like 'ext-ms-*' -or (Test-Path -LiteralPath (Join-Path $system32 $dep))) {
            continue
        } else {
            throw "$file needs $dep, which is neither beside the build nor a system DLL"
        }
    }
}
$resources = Join-Path $Repo 'resources'
Copy-Item -LiteralPath $resources -Destination (Join-Path $StageApp 'resources') -Recurse
Remove-Item -LiteralPath (Join-Path $StageApp 'resources\fonts\README.md') -ErrorAction SilentlyContinue
Write-Host "  staged $((Get-ChildItem -LiteralPath $StageApp -File).Count) files, $(Get-DirSizeMB $StageApp) MB"

# --- 2. AI: Python runtime --------------------------------------------------------
$Lock = Join-Path $SidecarSrc 'requirements.lock'
$Sam2Lock = Join-Path $SidecarSrc 'requirements-sam2.lock'
$PyDir = Join-Path $StageAi 'python'
$SitePackages = Join-Path $PyDir 'Lib\site-packages'
$Stamp = Join-Path $PyDir '.qm-stage-stamp'
$stampValue = "$PythonZipSha256 $(Get-Sha256 $Lock) $(Get-Sha256 $Sam2Lock) pip=$PipVersion v1"

if ((Test-Path -LiteralPath $Stamp) -and ((Get-Content -Raw -LiteralPath $Stamp).Trim() -eq $stampValue)) {
    Write-Host "python: reusing $PyDir (locks unchanged)"
} else {
    $downloads = Join-Path $Stage 'downloads'
    New-Item -ItemType Directory -Force -Path $downloads | Out-Null
    $zip = Join-Path $downloads "python-$PythonVersion-embed-amd64.zip"
    if (-not ((Test-Path -LiteralPath $zip) -and ((Get-Sha256 $zip) -eq $PythonZipSha256))) {
        Write-Host "python: download $PythonZipUrl"
        Invoke-Checked 'download embeddable Python' { curl.exe -fsSL --retry 3 -o $zip $PythonZipUrl }
        $actual = Get-Sha256 $zip
        if ($actual -ne $PythonZipSha256) {
            Remove-Item -LiteralPath $zip -Force
            throw "SHA-256 mismatch for the embeddable Python: expected $PythonZipSha256, got $actual"
        }
    }
    Reset-Dir $PyDir
    Expand-Archive -LiteralPath $zip -DestinationPath $PyDir

    # The ._pth replaces sys.path entirely (PYTHONPATH and PYTHONHOME are ignored, so a
    # user's own Python setup cannot leak in). Add site-packages, the sidecar folder (the
    # embeddable runtime does not put the script's folder on sys.path) and `import site`.
    $pth = Join-Path $PyDir 'python311._pth'
    if (-not (Test-Path -LiteralPath $pth)) { throw "python311._pth not found in $PyDir" }
    Set-Content -LiteralPath $pth -Encoding Ascii -Value @(
        'python311.zip', '.', 'Lib\site-packages', '..\sidecar', 'import site')

    # pip runs from a throwaway venv of the same Python 3.11 (the embeddable runtime has
    # no pip) and installs into the bundled site-packages with --target.
    $buildEnv = Join-Path $Stage 'buildenv'
    $buildPy = Join-Path $buildEnv 'Scripts\python.exe'
    if (-not (Test-Path -LiteralPath $buildPy)) {
        Invoke-Checked 'py -3.11 -m venv' { py -3.11 -m venv $buildEnv }
    }
    $hostVersion = & $buildPy -c "import sys; print('%d.%d' % sys.version_info[:2])"
    if ($hostVersion -ne '3.11') { throw "build venv Python is $hostVersion, expected 3.11. Delete $buildEnv." }
    Invoke-Checked 'pip upgrade' { & $buildPy -m pip install --disable-pip-version-check -q --upgrade "pip==$PipVersion" }

    New-Item -ItemType Directory -Force -Path $SitePackages | Out-Null
    Write-Host 'python: pip install requirements.lock (large: torch cu126)'
    Invoke-Checked 'pip install requirements.lock' {
        & $buildPy -m pip install --disable-pip-version-check -q --require-hashes --no-deps --target $SitePackages -r $Lock
    }
    # sam2 is source-only and its build imports torch: build it without isolation with
    # the bundled site-packages on PYTHONPATH. SAM2_BUILD_CUDA=0 skips the optional CUDA op.
    Write-Host 'python: pip install sam2 (built from the pinned sdist)'
    $savedPath = $env:PYTHONPATH
    $env:PYTHONPATH = $SitePackages
    $env:SAM2_BUILD_CUDA = '0'
    try {
        Invoke-Checked 'pip install sam2' {
            & $buildPy -m pip install --disable-pip-version-check -q --require-hashes --no-deps --no-build-isolation --target $SitePackages -r $Sam2Lock
        }
    } finally {
        $env:PYTHONPATH = $savedPath
        Remove-Item Env:SAM2_BUILD_CUDA -ErrorAction SilentlyContinue
    }

    # Console-script launchers point at the build venv's interpreter: useless here.
    Remove-Item -LiteralPath (Join-Path $SitePackages 'bin') -Recurse -Force -ErrorAction SilentlyContinue
    # Static import libraries (dnnl.lib alone is ~700 MB) are for building against torch,
    # never loaded at run time.
    Get-ChildItem -LiteralPath (Join-Path $SitePackages 'torch\lib') -Filter *.lib | Remove-Item -Force

    # The MSVC runtime beside python.exe too: torch's DLLs need msvcp140.dll, which the
    # embeddable package does not include.
    Get-ChildItem -LiteralPath $crtDir.FullName -Filter *.dll | Copy-Item -Destination $PyDir -Force

    Set-Content -LiteralPath $Stamp -Encoding Ascii -Value $stampValue
}

# --- 2b. AI: sidecar and models -----------------------------------------------------
$sidecarDst = Join-Path $StageAi 'sidecar'
Reset-Dir $sidecarDst
Get-ChildItem -LiteralPath $SidecarSrc -Filter *.py -File |
    Where-Object { $_.Name -notlike 'test_*' -and $_.Name -notlike 'gen_*' } |
    Copy-Item -Destination $sidecarDst
Write-Host "sidecar: $((Get-ChildItem -LiteralPath $sidecarDst).Name -join ', ')"

# Pins come from setup.ps1, the one place they are maintained.
$setupText = Get-Content -Raw -LiteralPath (Join-Path $SidecarSrc 'setup.ps1')
$pins = [regex]::Matches($setupText, "Path\s*=\s*'([^']+)'[\s\S]*?Sha256\s*=\s*'([0-9a-f]{64})'")
if ($pins.Count -eq 0) { throw 'No model pins found in setup.ps1' }
$modelsDst = Join-Path $StageAi 'models'
Reset-Dir $modelsDst
Write-Host "models: from $ModelsDir"
foreach ($pin in $pins) {
    $rel = $pin.Groups[1].Value
    $src = Join-Path $ModelsDir $rel
    if (-not (Test-Path -LiteralPath $src)) { throw "Model $rel missing from $ModelsDir. Run tools/ai-sidecar/setup.ps1 first." }
    $actual = Get-Sha256 $src
    if ($actual -ne $pin.Groups[2].Value) { throw "SHA-256 mismatch for model $rel`: expected $($pin.Groups[2].Value), got $actual" }
    $dst = Join-Path $modelsDst $rel
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
    Copy-Item -LiteralPath $src -Destination $dst
    Write-Host "  ok $rel"
}

# --- 2c. AI: check and precompile with the bundled interpreter ----------------------
$bundledPy = Join-Path $PyDir 'python.exe'
Invoke-Checked 'bundled import check' {
    & $bundledPy -c "import torch, torchvision, sam2, transformers, cv2; cv2.structured_light; cv2.aruco; import sidecar, models; print('bundled: torch', torch.__version__, '| cv2', cv2.__version__, '| transformers', transformers.__version__)"
}
# Bytecode is written now, as unchecked-hash .pyc, because Program Files is read-only
# at run time and installed timestamps are not guaranteed to match.
Write-Host 'python: precompiling bytecode'
$saved = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& $bundledPy -m compileall -q -j 0 --invalidation-mode unchecked-hash $SitePackages $sidecarDst | Out-Null
$ErrorActionPreference = $saved
if ($LASTEXITCODE -ne 0) { Write-Host '  (some files did not compile; they are templates or Python 2 sources and are not imported)' }

# --- 3. notices ----------------------------------------------------------------------
. (Join-Path $Packaging 'notices.ps1')
$vcpkgShare = $null
$cache = Join-Path $Repo 'build\release\CMakeCache.txt'
if (Test-Path -LiteralPath $cache) {
    $line = Select-String -LiteralPath $cache -Pattern '^VCPKG_INSTALLED_DIR:\w+=(.+)$' | Select-Object -First 1
    if ($line) { $vcpkgShare = Join-Path $line.Matches[0].Groups[1].Value 'x64-windows\share' }
}
if (-not $vcpkgShare -or -not (Test-Path -LiteralPath $vcpkgShare)) {
    $vcpkgShare = Join-Path $Repo 'build\release\vcpkg_installed\x64-windows\share'
}
if (-not (Test-Path -LiteralPath $vcpkgShare)) { throw "vcpkg share folder not found ($vcpkgShare)" }
$notices = Join-Path $StageApp 'THIRD_PARTY_NOTICES.txt'
Write-Notices -Path $notices -Repo $Repo -VcpkgShare $vcpkgShare -PyDir $PyDir `
    -SitePackages $SitePackages -CrtDir $crtDir.FullName -PythonVersion $PythonVersion
Write-Host "notices: $notices ($([math]::Round((Get-Item $notices).Length / 1KB)) KB)"

$appMB = Get-DirSizeMB $StageApp
$aiMB = Get-DirSizeMB $StageAi
Write-Host "staged: app $appMB MB, ai $aiMB MB (uncompressed)"
if ($StageOnly) { return }

# --- 4. installer --------------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $Out | Out-Null
Invoke-Checked 'ISCC' {
    & $iscc /Qp "/DAppVersion=$Version" "/DStageDir=$Stage" "/DOutputDir=$Out" (Join-Path $Packaging 'QUADmapper.iss')
}
Get-ChildItem -LiteralPath $Out -Filter "QUADmapper-Setup-$Version*" | ForEach-Object {
    Write-Host ("installer: {0} ({1:N0} MB)" -f $_.FullName, ($_.Length / 1MB))
}
