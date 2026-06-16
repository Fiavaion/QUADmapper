<#
.SYNOPSIS
  AI "Detect Surface" smoke (QM-058). Launches the app with the QM_AI_SELFTEST
  hook so aiDetectSurface() runs headlessly on the default quad (no fragile ImGui
  menu-driving), then verifies via the app's stdout log (ground truth) + a
  screenshot of the applied warp and the F11 projector output.

.DESCRIPTION
  QM_AI_TEST_IMAGE selects the scene, exercising the two AC paths:
    - test_screen.png -> high confidence -> "surface applied" (AC1) + F11 (AC3)
    - test_wall.png   -> floor-trap     -> "low confidence ... tap the surface" (AC2)
  A sidecar is pre-warmed so the in-app detect is fast; the self-test attaches to it.
#>
[CmdletBinding()]
param(
    [string]$ExePath       = "$PSScriptRoot/../../build/debug/src/app/Debug/QUADmapper.exe",
    [string]$OutDir        = "$PSScriptRoot/out_ai",
    [string]$TestImage     = "$PSScriptRoot/../../docs/research/snap-spike-evidence/test_screen.png",
    [string]$SidecarScript = "$PSScriptRoot/../../tools/ai-sidecar/sidecar.py",
    [string]$PythonExe     = "python",
    [int]$Port             = 8765,
    [string]$Tag           = "screen",
    [string]$SelfTestMode  = "1"     # "1" = Detect Surface; "oneflash" = QM-059 flow
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class W {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte sc, uint f, int e);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint c, uint t);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[W]::SetProcessDPIAware() | Out-Null

if (-not (Test-Path $ExePath))   { throw "exe not found: $ExePath" }
if (-not (Test-Path $TestImage)) { throw "test image not found: $TestImage" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$TestImage = (Resolve-Path $TestImage).Path
Write-Host "[ai-smoke] tag=$Tag image=$TestImage"

function Focus($h) {
    [W]::keybd_event(0x12, [byte][W]::MapVirtualKey(0x12, 0), 0, 0)
    [W]::keybd_event(0x12, [byte][W]::MapVirtualKey(0x12, 0), 0x0002, 0)
    [W]::ShowWindow($h, 9) | Out-Null
    [W]::BringWindowToTop($h) | Out-Null
    [W]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 300
}
function Capture($name, $bounds) {
    $bmp = New-Object System.Drawing.Bitmap($bounds.Width, $bounds.Height)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
    $bmp.Save((Join-Path $OutDir $name), [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Host "[ai-smoke] captured $name"
}

# ---- Pre-warm a sidecar (the in-app self-test attaches to it) ----
$sidecar = Start-Process -FilePath $PythonExe -ArgumentList @($SidecarScript, "--port", "$Port") `
    -PassThru -RedirectStandardOutput (Join-Path $OutDir "sidecar_out.log") `
    -RedirectStandardError (Join-Path $OutDir "sidecar_err.log")
Start-Sleep -Seconds 2
try {
    $body = (@{ image_path = $TestImage } | ConvertTo-Json -Compress)
    Invoke-RestMethod -Uri "http://127.0.0.1:$Port/surface" -Method Post -Body $body `
        -ContentType "application/json" -TimeoutSec 120 | Out-Null
    Write-Host "[ai-smoke] sidecar warmed."
} catch { Write-Host "[ai-smoke] warm failed (continuing): $_" }

# ---- Launch the app with the self-test hook armed ----
$env:QM_AI_SELFTEST  = $SelfTestMode
$env:QM_AI_TEST_IMAGE = $TestImage
$logFile = Join-Path $OutDir "app_$Tag.log"
$proc = Start-Process -FilePath $ExePath -PassThru -RedirectStandardOutput $logFile `
    -RedirectStandardError (Join-Path $OutDir "app_${Tag}_err.log")
try {
    $deadline = (Get-Date).AddSeconds(15)
    while (($proc.MainWindowHandle -eq 0) -and ((Get-Date) -lt $deadline)) {
        Start-Sleep -Milliseconds 200; $proc.Refresh()
    }
    if ($proc.MainWindowHandle -eq 0) { throw "app window never appeared" }
    $h = $proc.MainWindowHandle
    Start-Sleep -Seconds 6        # self-test runs at frame 3 + detect
    Focus $h
    $screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    Capture "ai_${Tag}_applied.png" $screen

    # In oneflash mode the self-test already entered perform mode (flashed the
    # pattern); otherwise tap F11 to project the AI-applied warp.
    if ($SelfTestMode -ne "oneflash") {
        $sc = [W]::MapVirtualKey(0x7A, 0)
        [W]::keybd_event(0x7A, [byte]$sc, 0x0008, 0); Start-Sleep -Milliseconds 15
        [W]::keybd_event(0x7A, [byte]$sc, 0x0008 -bor 0x0002, 0)
    }
    Start-Sleep -Seconds 2
    Capture "ai_${Tag}_projector.png" ([System.Windows.Forms.SystemInformation]::VirtualScreen)
}
finally {
    if ($proc -and -not $proc.HasExited) {
        $proc.CloseMainWindow() | Out-Null; Start-Sleep -Milliseconds 500
        if (-not $proc.HasExited) { $proc.Kill() }
    }
    if ($sidecar -and -not $sidecar.HasExited) { $sidecar.Kill() }
    Remove-Item Env:\QM_AI_SELFTEST   -ErrorAction SilentlyContinue
    Remove-Item Env:\QM_AI_TEST_IMAGE -ErrorAction SilentlyContinue
}

Write-Host "`n[ai-smoke] ===== app log: AI / One-Flash status ====="
Select-String -Path $logFile -Pattern "AI:","One-Flash:" | ForEach-Object { $_.Line }
Write-Host "[ai-smoke] DONE ($Tag). Screenshots in $OutDir"
