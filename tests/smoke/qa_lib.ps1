<#
.SYNOPSIS
  Step-by-step GUI driving helpers for QUADmapper manual/agent QA.

.DESCRIPTION
  Dot-source this file from each shell call:  . tests/smoke/qa_lib.ps1
  The app process persists between calls, so a QA session is a sequence of small
  commands (launch, click, key, capture) with the captures read back in between.
  Coordinates are PHYSICAL pixels of the primary monitor (the process opts into
  DPI awareness). Arrow keys are sent as extended scancodes (LESSON-TEST-004).

  Out dir: $env:QM_QA_OUT (default tests/smoke/out_qa).
#>
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
. "$PSScriptRoot/screen_capture.ps1"   # not CopyFromScreen; see that file
if (-not ([System.Management.Automation.PSTypeName]'QaNative').Type) {
    Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class QaNative {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint dwFlags, uint dx, uint dy, int dwData, int dwExtraInfo);
    [DllImport("user32.dll")] public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, int dwExtraInfo);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint uCode, uint uMapType);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr hWnd, int x, int y, int w, int h, bool repaint);
}
"@
}
[QaNative]::SetProcessDPIAware() | Out-Null

$QaRoot = (Resolve-Path "$PSScriptRoot/../..").Path
$QaExe  = Join-Path $QaRoot 'build/debug/src/app/Debug/QUADmapper.exe'
$QaOut  = if ($env:QM_QA_OUT) { $env:QM_QA_OUT } else { Join-Path $PSScriptRoot 'out_qa' }
New-Item -ItemType Directory -Force -Path $QaOut | Out-Null
$QaFrameMs = 50

$VK = @{ TAB=0x09; ENTER=0x0D; SHIFT=0x10; CTRL=0x11; ALT=0x12; ESC=0x1B; SPACE=0x20
         LEFT=0x25; UP=0x26; RIGHT=0x27; DOWN=0x28; DEL=0x2E; HOME=0x24; END=0x23; BACK=0x08
         A=0x41; N=0x4E; O=0x4F; S=0x53; V=0x56; Z=0x5A; F11=0x7A }
$QaExtended = @(0x25,0x26,0x27,0x28,0x2E,0x24,0x23)

function Get-QaProc { Get-Process QUADmapper -ErrorAction SilentlyContinue | Select-Object -First 1 }
# The editor's GLFW window (a Debug build also owns a console window, which
# Process.MainWindowHandle sometimes returns instead).
function Get-QaWindow { [QaNative]::FindWindow('GLFW30', 'QUADmapper') }

function Start-Qa([string]$Name = 'run', [string[]]$AppArgs = @()) {
    if (Get-QaProc) { throw "QUADmapper already running" }
    $p = @{ FilePath = $QaExe; PassThru = $true; WorkingDirectory = $QaRoot
            RedirectStandardOutput = (Join-Path $QaOut "${Name}_stdout.log")
            RedirectStandardError  = (Join-Path $QaOut "${Name}_stderr.log") }
    if ($AppArgs.Count) { $p.ArgumentList = $AppArgs }
    $proc = Start-Process @p
    $deadline = (Get-Date).AddSeconds(20)
    while (($proc.MainWindowHandle -eq 0) -and ((Get-Date) -lt $deadline)) { Start-Sleep -Milliseconds 200; $proc.Refresh() }
    if ($proc.MainWindowHandle -eq 0) { throw "App window never appeared." }
    Start-Sleep -Seconds 2
    # Fixed window rect that stays clear of the taskbar: a maximized window here
    # covers the whole monitor and the taskbar hides the bottom of the dock panels.
    $deadline = (Get-Date).AddSeconds(10)
    while (((Get-QaWindow) -eq [IntPtr]::Zero) -and ((Get-Date) -lt $deadline)) { Start-Sleep -Milliseconds 200 }
    $hwnd = Get-QaWindow
    [QaNative]::ShowWindow($hwnd, 9) | Out-Null   # SW_RESTORE
    [QaNative]::MoveWindow($hwnd, 0, 0, 2560, 1360, $true) | Out-Null
    Focus-Qa
    Start-Sleep -Milliseconds 800
    "started pid $($proc.Id)"
}

function Focus-Qa {
    $hwnd = Get-QaWindow; if ($hwnd -eq [IntPtr]::Zero) { throw "editor window not found" }
    [QaNative]::SetForegroundWindow($hwnd) | Out-Null
    Start-Sleep -Milliseconds 150
}

function Stop-Qa {
    $proc = Get-QaProc
    if ($proc) { $proc.Kill(); $proc.WaitForExit(5000) | Out-Null; "killed" } else { "not running" }
}

function Move-QaCursor([int]$x, [int]$y) { [QaNative]::SetCursorPos($x, $y) | Out-Null; Start-Sleep -Milliseconds 60 }
function Click([int]$x, [int]$y, [switch]$Right) {
    Move-QaCursor $x $y
    if ($Right) { [QaNative]::mouse_event(0x0008,0,0,0,0); Start-Sleep -Milliseconds 30; [QaNative]::mouse_event(0x0010,0,0,0,0) }
    else        { [QaNative]::mouse_event(0x0002,0,0,0,0); Start-Sleep -Milliseconds 30; [QaNative]::mouse_event(0x0004,0,0,0,0) }
    Start-Sleep -Milliseconds 120
}
function DblClick([int]$x, [int]$y) {
    Move-QaCursor $x $y
    1..2 | ForEach-Object { [QaNative]::mouse_event(0x0002,0,0,0,0); Start-Sleep -Milliseconds 20; [QaNative]::mouse_event(0x0004,0,0,0,0); Start-Sleep -Milliseconds 60 }
    Start-Sleep -Milliseconds 120
}
function Drag([int]$x1, [int]$y1, [int]$x2, [int]$y2, [int]$Steps = 20) {
    Move-QaCursor $x1 $y1
    [QaNative]::mouse_event(0x0002,0,0,0,0); Start-Sleep -Milliseconds 80
    for ($i = 1; $i -le $Steps; $i++) {
        [QaNative]::SetCursorPos([int]($x1 + ($x2-$x1)*$i/$Steps), [int]($y1 + ($y2-$y1)*$i/$Steps)) | Out-Null
        Start-Sleep -Milliseconds 25
    }
    Start-Sleep -Milliseconds 80
    [QaNative]::mouse_event(0x0004,0,0,0,0); Start-Sleep -Milliseconds 120
}

function KeyDown([int]$vk) {
    $s = [QaNative]::MapVirtualKey([uint32]$vk, 0); $f = 0x0008; if ($QaExtended -contains $vk) { $f = $f -bor 0x0001 }
    [QaNative]::keybd_event([byte]$vk, [byte]$s, $f, 0)
}
function KeyUp([int]$vk) {
    $s = [QaNative]::MapVirtualKey([uint32]$vk, 0); $f = 0x000A; if ($QaExtended -contains $vk) { $f = $f -bor 0x0001 }
    [QaNative]::keybd_event([byte]$vk, [byte]$s, $f, 0)
}
function Tap([int]$vk, [int]$n = 1) {
    for ($i = 0; $i -lt $n; $i++) { KeyDown $vk; Start-Sleep -Milliseconds 15; KeyUp $vk; Start-Sleep -Milliseconds $QaFrameMs }
}
function Chord([int[]]$mods, [int]$vk, [int]$n = 1) {
    foreach ($m in $mods) { KeyDown $m; Start-Sleep -Milliseconds 15 }
    for ($i = 0; $i -lt $n; $i++) { KeyDown $vk; Start-Sleep -Milliseconds 15; KeyUp $vk; Start-Sleep -Milliseconds $QaFrameMs }
    [array]::Reverse($mods); foreach ($m in $mods) { KeyUp $m; Start-Sleep -Milliseconds 15 }
    Start-Sleep -Milliseconds $QaFrameMs
}
# Replace the focused ImGui text field's content with $text (Ctrl+A, then paste).
function Paste([string]$text, [switch]$Enter) {
    Set-Clipboard -Value $text
    Chord @($VK.CTRL) $VK.A; Chord @($VK.CTRL) $VK.V
    if ($Enter) { Tap $VK.ENTER }
}

function Cap([string]$name, [switch]$Virtual, [switch]$Mon2, [int[]]$Rect) {
    if ($Virtual) { $b = [System.Windows.Forms.SystemInformation]::VirtualScreen }
    elseif ($Mon2) { $b = ([System.Windows.Forms.Screen]::AllScreens | Where-Object { -not $_.Primary } | Select-Object -First 1).Bounds }
    elseif ($Rect) { $b = New-Object System.Drawing.Rectangle($Rect[0], $Rect[1], $Rect[2], $Rect[3]) }
    else { $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds }
    $path = Join-Path $QaOut $name
    Save-ScreenRect $b $path
    "captured $path ($($b.X),$($b.Y) $($b.Width)x$($b.Height))"
}
