<#
.SYNOPSIS
  Sets up the QUADmapper AI sidecar's own environment and models (ADR-053 section 1).

.DESCRIPTION
  1. Creates tools/ai-sidecar/.venv with Python 3.11 (py -3.11).
  2. Installs the pinned pip (requirements-pip.lock), then requirements.lock (every
     package pinned with hashes, --require-hashes) into it, then sam2 from
     requirements-sam2.lock. The global Python is never touched.
  3. Downloads the models from their official sources into
     %LOCALAPPDATA%\QUADmapper\models (or $env:QM_MODELS_DIR) and checks each file's
     pinned SHA-256. A mismatch deletes the file and stops the script.

  Safe to re-run: an existing venv is reused, pip skips satisfied pins, and a model
  file already present with the right hash is not downloaded again.

.PARAMETER Dev
  Also installs requirements-dev.lock (pytest), for running the sidecar's tests.
#>
[CmdletBinding()]
param([switch]$Dev)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Here = $PSScriptRoot
$Venv = Join-Path $Here '.venv'
$VenvPython = Join-Path $Venv 'Scripts\python.exe'
$Lock = Join-Path $Here 'requirements.lock'
$Sam2Lock = Join-Path $Here 'requirements-sam2.lock'
$DevLock = Join-Path $Here 'requirements-dev.lock'
$PipLock = Join-Path $Here 'requirements-pip.lock'
# Windows' own curl by its full path, not whichever curl.exe is first on PATH.
$Curl = Join-Path $env:SystemRoot 'System32\curl.exe'

if ($env:QM_MODELS_DIR) {
    $ModelsDir = $env:QM_MODELS_DIR
} else {
    $ModelsDir = Join-Path $env:LOCALAPPDATA 'QUADmapper\models'
}

# Official sources, pinned. Depth Anything V2 is the Small model only (Apache-2.0);
# Base, Large and Giant are CC-BY-NC and must never be added here.
$DepthRevision = '5426e4f0f36572d16453bbda7a8389317b1bef99'
$DepthBase = "https://huggingface.co/depth-anything/Depth-Anything-V2-Small-hf/resolve/$DepthRevision"
$Models = @(
    @{ Path = 'sam2.1_hiera_small.pt'
       Url = 'https://dl.fbaipublicfiles.com/segment_anything_2/092824/sam2.1_hiera_small.pt'
       Sha256 = '6d1aa6f30de5c92224f8172114de081d104bbd23dd9dc5c58996f0cad5dc4d38' },
    @{ Path = 'sam2.1_hiera_tiny.pt'
       Url = 'https://dl.fbaipublicfiles.com/segment_anything_2/092824/sam2.1_hiera_tiny.pt'
       Sha256 = '7402e0d864fa82708a20fbd15bc84245c2f26dff0eb43a4b5b93452deb34be69' },
    @{ Path = 'depth-anything-v2-small\config.json'
       Url = "$DepthBase/config.json"
       Sha256 = 'c56698d3643dde1f83ea2212759e6b31a22b8f827246a36dd007ee8a22b3ff75' },
    @{ Path = 'depth-anything-v2-small\preprocessor_config.json'
       Url = "$DepthBase/preprocessor_config.json"
       Sha256 = 'd41175c0d889477ca8fc67191e540faef14baf6275157b3fdecf78469e6bbf84' },
    @{ Path = 'depth-anything-v2-small\model.safetensors'
       Url = "$DepthBase/model.safetensors"
       Sha256 = '3152477ce0d8d6978d76b995120de97cb5b928701fd0f817769f59e249a16b70' }
)

function Invoke-Checked {
    param([string]$What, [scriptblock]$Command)
    # Windows PowerShell 5.1 turns a native tool's stderr (pip warnings) into a
    # terminating error under 'Stop' when output is redirected; judge by exit code.
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $Command } finally { $ErrorActionPreference = $saved }
    if ($LASTEXITCODE -ne 0) { throw "$What failed (exit code $LASTEXITCODE)" }
}

function Get-Sha256([string]$File) {
    (Get-FileHash -Algorithm SHA256 -LiteralPath $File).Hash.ToLowerInvariant()
}

# --- 1. venv -----------------------------------------------------------------
if (Test-Path -LiteralPath $VenvPython) {
    Write-Host "venv: reusing $Venv"
} else {
    Write-Host "venv: creating $Venv with Python 3.11"
    Invoke-Checked 'py -3.11 -m venv' { py -3.11 -m venv $Venv }
}
$version = & $VenvPython -c "import sys; print('%d.%d' % sys.version_info[:2])"
if ($version -ne '3.11') { throw "venv Python is $version, expected 3.11. Delete $Venv and re-run." }

# --- 2. packages ---------------------------------------------------------------
# Everything except sam2 first, from the hash-locked requirements.lock (it carries the
# PyTorch cu126 index for torch and torchvision). sam2 is source-only and its build
# imports the torch installed here, so it is its own step, from the sdist hash in
# requirements-sam2.lock, built without isolation. SAM2_BUILD_CUDA=0 skips its
# optional CUDA op (mask hole filling), which would need the CUDA toolkit.
Invoke-Checked 'pip upgrade' {
    & $VenvPython -m pip install --disable-pip-version-check -q --require-hashes --no-deps -r $PipLock
}
Invoke-Checked 'pip install requirements.lock' {
    & $VenvPython -m pip install --disable-pip-version-check -q --require-hashes --no-deps -r $Lock
}
$env:SAM2_BUILD_CUDA = '0'
Invoke-Checked 'pip install sam2' {
    & $VenvPython -m pip install --disable-pip-version-check -q --require-hashes --no-deps --no-build-isolation -r $Sam2Lock
}
if ($Dev) {
    Invoke-Checked 'pip install requirements-dev.lock' {
        & $VenvPython -m pip install --disable-pip-version-check -q --require-hashes --no-deps -r $DevLock
    }
}
Invoke-Checked 'import check' {
    & $VenvPython -c "import torch, torchvision, sam2, transformers, cv2; cv2.structured_light; cv2.aruco; print('packages: torch', torch.__version__, 'cuda', torch.cuda.is_available(), '| cv2', cv2.__version__, '| transformers', transformers.__version__)"
}

# --- 3. models -----------------------------------------------------------------
Write-Host "models: $ModelsDir"
foreach ($m in $Models) {
    $dest = Join-Path $ModelsDir $m.Path
    if ((Test-Path -LiteralPath $dest) -and ((Get-Sha256 $dest) -eq $m.Sha256)) {
        Write-Host "  ok       $($m.Path)"
        continue
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
    $part = "$dest.part"
    Write-Host "  download $($m.Path)"
    # HTTPS only, also after a redirect.
    Invoke-Checked "download $($m.Url)" {
        & $Curl --proto '=https' --proto-redir '=https' -fsSL --retry 3 -o $part $m.Url
    }
    $actual = Get-Sha256 $part
    if ($actual -ne $m.Sha256) {
        Remove-Item -LiteralPath $part -Force
        throw "SHA-256 mismatch for $($m.Path): expected $($m.Sha256), got $actual. The file was deleted."
    }
    Move-Item -LiteralPath $part -Destination $dest -Force
    Write-Host "  ok       $($m.Path)"
}

Write-Host "setup complete. Run the sidecar with: $VenvPython $(Join-Path $Here 'sidecar.py')"
