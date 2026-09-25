<#
.SYNOPSIS
  Media-assignment smoke test for QUADmapper: loads a real video onto a quad and
  verifies it renders on the quad AND on the projector output (F11). Complements
  drive_smoke.ps1 (which only tests warp + projector with a white quad).

.DESCRIPTION
  Drives the REAL UI: types a video path into the Video Player panel, clicks
  Load Video, Assign to Selected Quad, Play, then warps a corner and sends the
  result fullscreen to the secondary monitor. Captures screenshots so the video
  frame can be visually confirmed on the quad and on the projector.

  Coordinates for the deeper panel buttons (Play/Assign) are approximate; the
  03_loaded.png capture exists to reveal their true positions for tuning.
#>
[CmdletBinding()]
param(
    [string]$ExePath   = "$PSScriptRoot/../../build/debug/src/app/Debug/QUADmapper.exe",
    [string]$OutDir    = "$PSScriptRoot/out_media",
    [string]$VideoPath = "D:\testing Videos\Media\testy.mp4",   # 30 fps, 20 s
    [int]$FrameMs      = 50,
    # Tunable click points (physical px; the window is placed at 0,0 2560x1360 on the
    # primary monitor). Adjust after reading 02_path_typed.png / 03_loaded.png.
    [int]$PathX    = 228, [int]$PathY    = 170,
    [int]$UsePathX = 360, [int]$UsePathY = 170,
    [int]$AssignX  = 123, [int]$AssignY  = 553,
    [int]$PlayX    = 50,  [int]$PlayY    = 315,
    # Video Player "Time: x / y s" readout, captured twice ~5 s apart (QM-088 real-speed check).
    [int[]]$TimeRect = @(0, 370, 400, 30)
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

if (-not ([System.Management.Automation.PSTypeName]'Win32Native').Type) {
    Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win32Native {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint dwFlags, uint dx, uint dy, uint dwData, int dwExtraInfo);
    [DllImport("user32.dll")] public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, int dwExtraInfo);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint uCode, uint uMapType);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr hWnd, int x, int y, int w, int h, bool repaint);
}
"@
}
[Win32Native]::SetProcessDPIAware() | Out-Null

if (-not (Test-Path $ExePath))   { throw "exe not found: $ExePath" }
if (-not (Test-Path $VideoPath)) { throw "video not found: $VideoPath" }
$ExePath = (Resolve-Path $ExePath).Path
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
Write-Host "[media] exe   : $ExePath"
Write-Host "[media] video : $VideoPath"
Write-Host "[media] outdir: $OutDir"

# The editor's GLFW window: a Debug build also owns a console window, which
# Process.MainWindowHandle sometimes returns instead.
function Get-EditorWindow { [Win32Native]::FindWindow('GLFW30', 'QUADmapper') }
function Place-App {
    # A fixed rect clear of the taskbar: a maximized window here covers the whole
    # monitor, and the taskbar hides the bottom of the dock panels.
    $h = Get-EditorWindow
    [Win32Native]::ShowWindow($h, 9) | Out-Null   # SW_RESTORE
    [Win32Native]::MoveWindow($h, 0, 0, 2560, 1360, $true) | Out-Null
    Start-Sleep -Milliseconds 500
}
function Focus-App($proc) {
    [Win32Native]::SetForegroundWindow((Get-EditorWindow)) | Out-Null
    Start-Sleep -Milliseconds 200
}
function Capture([string]$name, [System.Drawing.Rectangle]$bounds) {
    $bmp = New-Object System.Drawing.Bitmap($bounds.Width, $bounds.Height)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
    $bmp.Save((Join-Path $OutDir $name), [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Host "[media] captured $name ($($bounds.Width)x$($bounds.Height))"
}
function Click([int]$x, [int]$y) {
    [Win32Native]::SetCursorPos($x, $y) | Out-Null
    Start-Sleep -Milliseconds 80
    [Win32Native]::mouse_event(0x0002, 0, 0, 0, 0)
    [Win32Native]::mouse_event(0x0004, 0, 0, 0, 0)
    Start-Sleep -Milliseconds $FrameMs
}
$VK = @{ TAB=0x09; SHIFT=0x10; CTRL=0x11; V=0x56; LEFT=0x25; UP=0x26; RIGHT=0x27; DOWN=0x28; F11=0x7A }
$EXT = @($VK.LEFT,$VK.UP,$VK.RIGHT,$VK.DOWN)
$SC=0x0008; $KU=0x0002; $EK=0x0001
function KeyDown([int]$vk){ $s=[Win32Native]::MapVirtualKey([uint32]$vk,0); $f=$SC; if($EXT -contains $vk){$f=$f -bor $EK}; [Win32Native]::keybd_event([byte]$vk,[byte]$s,$f,0) }
function KeyUp([int]$vk){ $s=[Win32Native]::MapVirtualKey([uint32]$vk,0); $f=$SC -bor $KU; if($EXT -contains $vk){$f=$f -bor $EK}; [Win32Native]::keybd_event([byte]$vk,[byte]$s,$f,0) }
function Tap([int]$vk,[int]$n=1){ for($i=0;$i -lt $n;$i++){ KeyDown $vk; Start-Sleep -Milliseconds 15; KeyUp $vk; Start-Sleep -Milliseconds $FrameMs } }
function Chord([int]$m,[int]$vk,[int]$n=1){ KeyDown $m; Start-Sleep -Milliseconds 15; for($i=0;$i -lt $n;$i++){ KeyDown $vk; Start-Sleep -Milliseconds 15; KeyUp $vk; Start-Sleep -Milliseconds $FrameMs }; KeyUp $m; Start-Sleep -Milliseconds $FrameMs }

Set-Clipboard -Value $VideoPath

Write-Host "[media] launching..."
$proc = Start-Process -FilePath $ExePath -PassThru `
    -RedirectStandardOutput (Join-Path $OutDir "app_stdout.log") `
    -RedirectStandardError  (Join-Path $OutDir "app_stderr.log")
try {
    $deadline = (Get-Date).AddSeconds(15)
    while (($proc.MainWindowHandle -eq 0) -and ((Get-Date) -lt $deadline)) { Start-Sleep -Milliseconds 200; $proc.Refresh() }
    if ($proc.MainWindowHandle -eq 0) { throw "App window never appeared." }
    Start-Sleep -Seconds 2
    Place-App
    Focus-App $proc
    $screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $px = { param($x) $screen.X + $x }
    $py = { param($y) $screen.Y + $y }

    Capture "01_initial.png" $screen

    # 1) Focus the video path field and paste the path (Ctrl+V into ImGui InputText).
    Click (& $px $PathX) (& $py $PathY)
    Chord $VK.CTRL $VK.V
    Start-Sleep -Milliseconds $FrameMs
    Capture "02_path_typed.png" $screen

    # 2) Click "Use Path" (right of the input field).
    Click (& $px $UsePathX) (& $py $UsePathY)
    Start-Sleep -Seconds 3   # let FFmpeg open + first decode
    Focus-App $proc
    Capture "03_loaded.png" $screen   # reveals true Play/Assign positions for tuning

    # 3) Assign the video to the selected quad, then Play so frames pump.
    Click (& $px $AssignX) (& $py $AssignY)
    Start-Sleep -Milliseconds 200
    Click (& $px $PlayX) (& $py $PlayY)
    Start-Sleep -Seconds 1
    Focus-App $proc
    Capture "04_assigned_playing.png" $screen   # quad should show the video frame

    # Real-speed check: the time readout must advance ~5 s over ~5 s of wall clock.
    $timeBounds = New-Object System.Drawing.Rectangle($TimeRect[0], $TimeRect[1], $TimeRect[2], $TimeRect[3])
    $t1 = Get-Date; Capture "04a_time_t0.png" $timeBounds
    Start-Sleep -Seconds 5
    $t2 = Get-Date; Capture "04b_time_t5.png" $timeBounds
    Write-Host ("[media] wall clock between time captures: {0:N2} s" -f ($t2 - $t1).TotalSeconds)

    # 4) Warp a corner so it's unmistakably the mapped quad, then send to projector.
    # No canvas click first: with a quad selected the canvas owns Tab/arrows (QM-089).
    Focus-App $proc
    Tap $VK.TAB
    Chord $VK.SHIFT $VK.RIGHT 15
    Chord $VK.SHIFT $VK.DOWN 8
    Focus-App $proc
    Capture "05_warped_video.png" $screen

    Tap $VK.F11
    Start-Sleep -Seconds 2
    Capture "06_projector_video.png" ([System.Windows.Forms.SystemInformation]::VirtualScreen)

    Write-Host "[media] sequence complete."
}
finally {
    if ($proc -and -not $proc.HasExited) {
        $proc.CloseMainWindow() | Out-Null
        Start-Sleep -Milliseconds 500
        if (-not $proc.HasExited) { $proc.Kill() }
    }
    Write-Host "[media] app closed."
}
Write-Host "[media] DONE. Screenshots in $OutDir"
