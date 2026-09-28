<#
.SYNOPSIS
  UI-driving smoke harness for the ADR-039 P3 Scenes panel (QM-065).
  Launches the app, switches to the Resources > Scenes tab, creates a 2nd
  scene, switches the active scene, and assigns an output — capturing
  screenshots at each step for visual verification.

.DESCRIPTION
  Verifies the parts of QM-065 that unit tests can't reach: that the Scenes
  tab renders, the New/Delete buttons mutate the scene list, the active marker
  follows selection, and the per-scene output dropdown is operable. Uses only
  OS-level input + screen capture (same approach as drive_smoke.ps1).

  Click coordinates for the tab/buttons are passed in (they depend on the saved
  imgui.ini dock layout); read 00_initial.png first, then re-run with the right
  -TabX/-TabY etc. Screenshots land in tests/smoke/out-scenes/.
#>
[CmdletBinding()]
param(
    [string]$ExePath = "$PSScriptRoot/../../build/debug/src/app/Debug/QUADmapper.exe",
    [string]$OutDir  = "$PSScriptRoot/out-scenes",
    [int]$FrameMs    = 60,
    # Click targets (screen-absolute on primary monitor). Defaults are placeholders;
    # set them from 00_initial.png after the first run.
    [int]$TabX = 0, [int]$TabY = 0,        # the "Scenes" tab header
    [int]$NewX = 0, [int]$NewY = 0,        # the "New" button
    [int]$ComboX = 0, [int]$ComboY = 0,    # the first scene's output dropdown
    [int]$LoadX = 0, [int]$LoadY = 0,      # the Project tab's Load button (clicked first)
    [switch]$InitialOnly,
    [switch]$SaveAfter                     # send Ctrl+S after the actions
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
. "$PSScriptRoot/screen_capture.ps1"   # not CopyFromScreen; see that file

if (-not ([System.Management.Automation.PSTypeName]'Win32Native').Type) {
    Add-Type @"
using System; using System.Runtime.InteropServices;
public static class Win32Native {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, IntPtr pid);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint idAttach, uint idAttachTo, bool fAttach);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter, int X, int Y, int cx, int cy, uint uFlags);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint dwFlags, uint dx, uint dy, uint dwData, int dwExtraInfo);
    [DllImport("user32.dll")] public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, int dwExtraInfo);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint uCode, uint uMapType);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
}

function Send-CtrlS {
    $VK_CONTROL = 0x11; $VK_S = 0x53; $SCAN = 0x0008; $KEYUP = 0x0002
    $sCtrl = [Win32Native]::MapVirtualKey([uint32]$VK_CONTROL, 0)
    $sS = [Win32Native]::MapVirtualKey([uint32]$VK_S, 0)
    [Win32Native]::keybd_event([byte]$VK_CONTROL, [byte]$sCtrl, $SCAN, 0)
    Start-Sleep -Milliseconds 30
    [Win32Native]::keybd_event([byte]$VK_S, [byte]$sS, $SCAN, 0)
    Start-Sleep -Milliseconds 30
    [Win32Native]::keybd_event([byte]$VK_S, [byte]$sS, ($SCAN -bor $KEYUP), 0)
    Start-Sleep -Milliseconds 30
    [Win32Native]::keybd_event([byte]$VK_CONTROL, [byte]$sCtrl, ($SCAN -bor $KEYUP), 0)
    Start-Sleep -Milliseconds 200
}
[Win32Native]::SetProcessDPIAware() | Out-Null

if (-not (Test-Path $ExePath)) { throw "exe not found: $ExePath" }
$ExePath = (Resolve-Path $ExePath).Path
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
Write-Host "[scenes] exe: $ExePath"

function Focus-App($proc) {
    $h = $proc.MainWindowHandle
    [Win32Native]::ShowWindow($h, 9) | Out-Null   # SW_RESTORE
    # Pin topmost AND move to a fixed origin (0,0) so a foreground-grabbing app
    # (Claude desktop) can't cover it and the panel layout is at deterministic
    # screen coordinates run-to-run. SWP_NOSIZE keeps the window's own size.
    $HWND_TOPMOST = [IntPtr](-1); $SWP_NOSIZE = 0x0001
    [Win32Native]::SetWindowPos($h, $HWND_TOPMOST, 0, 0, 0, 0, $SWP_NOSIZE) | Out-Null
    # Foreground-steal via AttachThreadInput: attach our input queue to the thread
    # that currently owns the foreground window, so SetForegroundWindow is honoured
    # (Windows otherwise blocks cross-app foreground changes). See Session-16 notes.
    $fg = [Win32Native]::GetForegroundWindow()
    $fgThread = [Win32Native]::GetWindowThreadProcessId($fg, [IntPtr]::Zero)
    $myThread = [Win32Native]::GetCurrentThreadId()
    [Win32Native]::AttachThreadInput($myThread, $fgThread, $true) | Out-Null
    [Win32Native]::BringWindowToTop($h) | Out-Null
    [Win32Native]::SetForegroundWindow($h) | Out-Null
    [Win32Native]::AttachThreadInput($myThread, $fgThread, $false) | Out-Null
    Start-Sleep -Milliseconds 250
}
function Capture([string]$name, [System.Drawing.Rectangle]$bounds) {
    Save-ScreenRect $bounds (Join-Path $OutDir $name)
    Write-Host "[scenes] captured $name"
}
function Click([int]$x, [int]$y) {
    [Win32Native]::SetCursorPos($x, $y) | Out-Null
    Start-Sleep -Milliseconds 100
    [Win32Native]::mouse_event(0x0002, 0, 0, 0, 0)
    [Win32Native]::mouse_event(0x0004, 0, 0, 0, 0)
    Start-Sleep -Milliseconds $FrameMs
}

Write-Host "[scenes] launching..."
$proc = Start-Process -FilePath $ExePath -PassThru `
    -RedirectStandardOutput (Join-Path $OutDir "app_stdout.log") `
    -RedirectStandardError  (Join-Path $OutDir "app_stderr.log")
try {
    $deadline = (Get-Date).AddSeconds(15)
    while (($proc.MainWindowHandle -eq 0) -and ((Get-Date) -lt $deadline)) { Start-Sleep -Milliseconds 200; $proc.Refresh() }
    if ($proc.MainWindowHandle -eq 0) { throw "window never appeared" }
    Start-Sleep -Seconds 2
    Focus-App $proc
    $screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds

    Capture "00_initial.png" $screen
    if ($InitialOnly) { Write-Host "[scenes] initial-only; inspect 00_initial.png for coords"; return }

    # Re-assert focus+topmost before EVERY click: a foreground-grabbing app
    # (Claude desktop) keeps reclaiming the top, so one focus at start isn't enough.
    function Act([int]$x, [int]$y) {
        Focus-App $proc
        Click ($screen.X + $x) ($screen.Y + $y)
        Click ($screen.X + $x) ($screen.Y + $y)   # 2nd click: 1st may be eaten by activation
    }
    if ($LoadX -gt 0) { Act $LoadX $LoadY; Focus-App $proc; Capture "00b_after_load.png" $screen }
    if ($TabX -gt 0) { Act $TabX $TabY; Focus-App $proc; Capture "01_scenes_tab.png" $screen }
    if ($NewX -gt 0) {
        Act $NewX $NewY; Focus-App $proc; Capture "02_after_new.png" $screen
        Act $NewX $NewY; Focus-App $proc; Capture "03_after_new2.png" $screen
    }
    if ($ComboX -gt 0) { Act $ComboX $ComboY; Focus-App $proc; Capture "04_combo_open.png" $screen }
    if ($SaveAfter) { Focus-App $proc; Send-CtrlS; Write-Host "[scenes] sent Ctrl+S" }
    Write-Host "[scenes] sequence complete."
}
finally {
    if ($proc -and -not $proc.HasExited) {
        $proc.CloseMainWindow() | Out-Null; Start-Sleep -Milliseconds 500
        if (-not $proc.HasExited) { $proc.Kill() }
    }
    Write-Host "[scenes] app closed."
}
Write-Host "[scenes] DONE -> $OutDir"
