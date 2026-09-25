<#
.SYNOPSIS
  Image-assignment smoke test for QUADmapper: loads a still image onto a quad via
  the Inspector "Texture: Path/Load" field and verifies it renders on the quad AND
  on the projector output (F11). Sibling to drive_media_smoke.ps1 (video).
#>
[CmdletBinding()]
param(
    [string]$ExePath   = "C:\Users\jones\AIprojects\QUADmapper\build\debug\src\app\Debug\QUADmapper.exe",
    [string]$OutDir    = "$PSScriptRoot/out_image",
    [string]$ImagePath = "D:\testing Videos\knowledge-graph.png",
    [int]$FrameMs      = 50,
    # Inspector Texture row (primary-screen-relative, maximized 2560-wide layout).
    [int]$PathX = 2270, [int]$PathY = 224,
    [int]$LoadX = 2497, [int]$LoadY = 224
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
. "$PSScriptRoot/screen_capture.ps1"   # not CopyFromScreen; see that file

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
}
"@
}
[Win32Native]::SetProcessDPIAware() | Out-Null

if (-not (Test-Path $ExePath))   { throw "exe not found: $ExePath" }
if (-not (Test-Path $ImagePath)) { throw "image not found: $ImagePath" }
$ExePath = (Resolve-Path $ExePath).Path
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
Write-Host "[image] exe   : $ExePath"
Write-Host "[image] image : $ImagePath"

function Focus-App($proc) {
    [Win32Native]::ShowWindow($proc.MainWindowHandle, 3) | Out-Null   # SW_MAXIMIZE
    [Win32Native]::SetForegroundWindow($proc.MainWindowHandle) | Out-Null
    Start-Sleep -Milliseconds 200
}
function Capture([string]$name, [System.Drawing.Rectangle]$bounds) {
    Save-ScreenRect $bounds (Join-Path $OutDir $name)
    Write-Host "[image] captured $name ($($bounds.Width)x$($bounds.Height))"
}
function Click([int]$x, [int]$y) {
    [Win32Native]::SetCursorPos($x, $y) | Out-Null
    Start-Sleep -Milliseconds 80
    [Win32Native]::mouse_event(0x0002, 0, 0, 0, 0); [Win32Native]::mouse_event(0x0004, 0, 0, 0, 0)
    Start-Sleep -Milliseconds $FrameMs
}
$VK = @{ TAB=0x09; SHIFT=0x10; CTRL=0x11; V=0x56; LEFT=0x25; UP=0x26; RIGHT=0x27; DOWN=0x28; F11=0x7A }
$EXT = @($VK.LEFT,$VK.UP,$VK.RIGHT,$VK.DOWN); $SC=0x0008; $KU=0x0002; $EK=0x0001
function KeyDown([int]$vk){ $s=[Win32Native]::MapVirtualKey([uint32]$vk,0); $f=$SC; if($EXT -contains $vk){$f=$f -bor $EK}; [Win32Native]::keybd_event([byte]$vk,[byte]$s,$f,0) }
function KeyUp([int]$vk){ $s=[Win32Native]::MapVirtualKey([uint32]$vk,0); $f=$SC -bor $KU; if($EXT -contains $vk){$f=$f -bor $EK}; [Win32Native]::keybd_event([byte]$vk,[byte]$s,$f,0) }
function Tap([int]$vk,[int]$n=1){ for($i=0;$i -lt $n;$i++){ KeyDown $vk; Start-Sleep -Milliseconds 15; KeyUp $vk; Start-Sleep -Milliseconds $FrameMs } }
function Chord([int]$m,[int]$vk,[int]$n=1){ KeyDown $m; Start-Sleep -Milliseconds 15; for($i=0;$i -lt $n;$i++){ KeyDown $vk; Start-Sleep -Milliseconds 15; KeyUp $vk; Start-Sleep -Milliseconds $FrameMs }; KeyUp $m; Start-Sleep -Milliseconds $FrameMs }

Set-Clipboard -Value $ImagePath

Write-Host "[image] launching..."
$proc = Start-Process -FilePath $ExePath -PassThru `
    -RedirectStandardOutput (Join-Path $OutDir "app_stdout.log") `
    -RedirectStandardError  (Join-Path $OutDir "app_stderr.log")
try {
    $deadline = (Get-Date).AddSeconds(15)
    while (($proc.MainWindowHandle -eq 0) -and ((Get-Date) -lt $deadline)) { Start-Sleep -Milliseconds 200; $proc.Refresh() }
    if ($proc.MainWindowHandle -eq 0) { throw "App window never appeared." }
    Start-Sleep -Seconds 2
    Focus-App $proc
    $screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $px = { param($x) $screen.X + $x }; $py = { param($y) $screen.Y + $y }

    Capture "01_initial.png" $screen

    # Paste image path into the Inspector Texture field, then click Load.
    Click (& $px $PathX) (& $py $PathY)
    Chord $VK.CTRL $VK.V
    Start-Sleep -Milliseconds $FrameMs
    Capture "02_path_typed.png" $screen
    Click (& $px $LoadX) (& $py $LoadY)
    Start-Sleep -Seconds 1
    Focus-App $proc
    Capture "03_image_on_quad.png" $screen   # quad should show the image

    # Warp a corner, then send to the projector.
    Click (& $px 1280) (& $py 200)   # clear ImGui nav focus on empty canvas
    Focus-App $proc
    Tap $VK.TAB
    Chord $VK.SHIFT $VK.RIGHT 15
    Chord $VK.SHIFT $VK.DOWN 8
    Focus-App $proc
    Capture "04_warped_image.png" $screen

    Tap $VK.F11
    Start-Sleep -Seconds 2
    Capture "05_projector_image.png" ([System.Windows.Forms.SystemInformation]::VirtualScreen)

    Write-Host "[image] sequence complete."
}
finally {
    if ($proc -and -not $proc.HasExited) {
        $proc.CloseMainWindow() | Out-Null
        Start-Sleep -Milliseconds 500
        if (-not $proc.HasExited) { $proc.Kill() }
    }
    Write-Host "[image] app closed."
}
Write-Host "[image] DONE. Screenshots in $OutDir"
