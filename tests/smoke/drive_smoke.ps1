<#
.SYNOPSIS
  UI-driving smoke harness for QUADmapper. Launches the app, drives the
  success-metric vertex-warp workflow via real keyboard input through ImGui,
  and captures screenshots for visual verification.

.DESCRIPTION
  This is the verification gate for the ADR-031 refactor: it exercises the parts
  of the success metric that unit tests cannot reach (the keyboard vertex-edit UX
  and, later, projector output). It uses only OS-level input + screen capture, so
  it needs no extra dependencies and tests the REAL input path (including the
  ImGui keyboard-capture guard).

  Default mode (CORNERS_ONLY) workflow driven:
    - {TAB} selects/cycles a corner
    - Shift+Arrow nudges the selected corner 10x (0.01 normalized per press)
    - Alt+Arrow nudges 0.1x (fine)
  Each press is spaced so ImGui sees it on a distinct frame.

  Screenshots land in tests/smoke/out/ (gitignored). Read them back to confirm
  the selected corner moves by the expected magnitude and direction.

.PARAMETER ExePath
  Path to QUADmapper.exe. Defaults to the debug build.

.PARAMETER OutDir
  Where to write screenshots. Defaults to tests/smoke/out.

.EXAMPLE
  pwsh -File tests/smoke/drive_smoke.ps1
#>
[CmdletBinding()]
param(
    [string]$ExePath = "$PSScriptRoot/../../build/debug/src/app/Debug/QUADmapper.exe",
    [string]$OutDir  = "$PSScriptRoot/out",
    [int]$FrameMs    = 50
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

# Make the process DPI-aware BEFORE querying screen metrics, so Screen bounds and
# CopyFromScreen use physical pixels (the secondary monitor runs at 150% scaling).

# Win32 helpers for focusing the GLFW window.
if (-not ([System.Management.Automation.PSTypeName]'Win32Native').Type) {
    Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win32Native {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
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

# Opt into physical-pixel coordinates (must run before any Screen query).
[Win32Native]::SetProcessDPIAware() | Out-Null

function Resolve-Required([string]$path, [string]$label) {
    if (-not (Test-Path $path)) { throw "$label not found: $path. Build first: cmake --build build/debug --target QUADmapper" }
    return (Resolve-Path $path).Path
}

$ExePath = Resolve-Required $ExePath "QUADmapper.exe"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
Write-Host "[smoke] exe   : $ExePath"
Write-Host "[smoke] outdir: $OutDir"

# The editor's GLFW window: a Debug build also owns a console window, which
# Process.MainWindowHandle sometimes returns instead.
function Get-EditorWindow { [Win32Native]::FindWindow('GLFW30', 'QUADmapper') }
function Place-App {
    # A fixed rect on the primary monitor, clear of the taskbar, so the Properties
    # corner values are on screen in every capture.
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
    $path = Join-Path $OutDir $name
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Host "[smoke] captured $name ($($bounds.Width)x$($bounds.Height))"
}

function Click([int]$x, [int]$y) {
    # Click on the passthru central canvas to clear ImGui widget/nav focus, so the
    # Tab-cycle-vertex feature wins over ImGui keyboard navigation.
    [Win32Native]::SetCursorPos($x, $y) | Out-Null
    Start-Sleep -Milliseconds 80
    [Win32Native]::mouse_event(0x0002, 0, 0, 0, 0)   # LEFTDOWN
    [Win32Native]::mouse_event(0x0004, 0, 0, 0, 0)   # LEFTUP
    Start-Sleep -Milliseconds $FrameMs
}

# Virtual-key codes
$VK = @{ TAB = 0x09; SHIFT = 0x10; ALT = 0x12; LEFT = 0x25; UP = 0x26; RIGHT = 0x27; DOWN = 0x28; F11 = 0x7A }
$KEYEVENTF_EXTENDEDKEY = 0x0001
$KEYEVENTF_KEYUP       = 0x0002
$KEYEVENTF_SCANCODE    = 0x0008
# GLFW's Win32 backend maps keys by SCANCODE (from WM_KEYDOWN lParam), not VK. keybd_event
# with bScan=0 makes extended arrow keys map to GLFW_KEY_UNKNOWN and get dropped, while the
# handler never sees them. So we send the real scancode (MapVirtualKey) with KEYEVENTF_SCANCODE,
# plus the extended flag for arrow keys. (See ADR-031 harness debugging.)
$ExtendedKeys = @($VK.LEFT, $VK.UP, $VK.RIGHT, $VK.DOWN)

function KeyDown([int]$vk) {
    $scan = [Win32Native]::MapVirtualKey([uint32]$vk, 0)   # MAPVK_VK_TO_VSC
    $flags = $KEYEVENTF_SCANCODE; if ($ExtendedKeys -contains $vk) { $flags = $flags -bor $KEYEVENTF_EXTENDEDKEY }
    [Win32Native]::keybd_event([byte]$vk, [byte]$scan, $flags, 0)
}
function KeyUp([int]$vk) {
    $scan = [Win32Native]::MapVirtualKey([uint32]$vk, 0)
    $flags = $KEYEVENTF_SCANCODE -bor $KEYEVENTF_KEYUP; if ($ExtendedKeys -contains $vk) { $flags = $flags -bor $KEYEVENTF_EXTENDEDKEY }
    [Win32Native]::keybd_event([byte]$vk, [byte]$scan, $flags, 0)
}
function Tap([int]$vk, [int]$times = 1) {
    for ($i = 0; $i -lt $times; $i++) { KeyDown $vk; Start-Sleep -Milliseconds 15; KeyUp $vk; Start-Sleep -Milliseconds $FrameMs }
}
function Chord([int]$modVk, [int]$vk, [int]$times = 1) {
    KeyDown $modVk; Start-Sleep -Milliseconds 15
    for ($i = 0; $i -lt $times; $i++) { KeyDown $vk; Start-Sleep -Milliseconds 15; KeyUp $vk; Start-Sleep -Milliseconds $FrameMs }
    KeyUp $modVk; Start-Sleep -Milliseconds $FrameMs
}

# ---- Launch ----
Write-Host "[smoke] launching..."
$logFile = Join-Path $OutDir "app_stdout.log"
$proc = Start-Process -FilePath $ExePath -PassThru -RedirectStandardOutput $logFile -RedirectStandardError (Join-Path $OutDir "app_stderr.log")
try {
    # Wait for the main window handle.
    $deadline = (Get-Date).AddSeconds(15)
    while (($proc.MainWindowHandle -eq 0) -and ((Get-Date) -lt $deadline)) {
        Start-Sleep -Milliseconds 200; $proc.Refresh()
    }
    if ($proc.MainWindowHandle -eq 0) { throw "App window never appeared." }
    Start-Sleep -Seconds 2   # let init + first frames settle
    Place-App
    Focus-App $proc

    # Capture bounds = the monitor the app window is on (primary).
    $screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds

    # ---- Drive the success-metric vertex workflow (CORNERS_ONLY default) ----
    Capture "01_initial.png" $screen

    # No canvas click first: with a quad selected the canvas owns Tab/arrows (QM-089).

    Tap $VK.TAB                                       # select corner 0 (top-left)
    Chord $VK.SHIFT $VK.RIGHT 20                      # Shift+Right x20 => +0.20 normalized (TL.x 0.300 -> 0.500)
    Chord $VK.SHIFT $VK.DOWN  10                      # Shift+Down  x10 => +0.10 normalized (TL.y 0.300 -> 0.400)
    Focus-App $proc
    Capture "02_corner0_shift_moved.png" $screen

    Tap $VK.TAB                                       # cycle to corner 1 (top-right)
    Chord $VK.SHIFT $VK.LEFT 15                       # Shift+Left x15 => -0.15 normalized (TR.x 0.700 -> 0.550)
    Focus-App $proc
    Capture "03_corner1_shift_moved.png" $screen

    Tap $VK.TAB                                       # cycle to corner 2 (bottom-right)
    Tap $VK.UP 20                                     # plain Up x20 => -0.020 normalized (BR.y 0.700 -> 0.680)
    Focus-App $proc
    Capture "04_corner2_plain_nudge.png" $screen

    # ---- TD-01: send the warped scene to the projector (F11 auto-creates an output) ----
    Tap $VK.F11                                       # F11 == send fullscreen to secondary monitor
    Start-Sleep -Seconds 2                            # let the output window appear + render
    # Capture the full virtual desktop (all monitors) — DPI-robust. A single-monitor
    # capture crops under per-monitor scaling; the virtual-desktop shot reliably shows
    # the complete projector content (left) next to the editor (right).
    Capture "05_projector_output.png" ([System.Windows.Forms.SystemInformation]::VirtualScreen)
    if (-not ([System.Windows.Forms.Screen]::AllScreens | Where-Object { -not $_.Primary })) {
        Write-Host "[smoke] NOTE: no secondary monitor; projector shows on primary"
    }

    Write-Host "[smoke] sequence complete."
}
finally {
    if ($proc -and -not $proc.HasExited) {
        $proc.CloseMainWindow() | Out-Null
        Start-Sleep -Milliseconds 500
        if (-not $proc.HasExited) { $proc.Kill() }
    }
    Write-Host "[smoke] app closed."
}

Write-Host "[smoke] DONE. Screenshots in $OutDir"
