<#
.SYNOPSIS
  Step-by-step GUI QA helpers for the ADR-052 3D projection-mapping feature
  (OBJ import, object Inspector, projector calibration).

.DESCRIPTION
  Dot-source it (it dot-sources qa_lib.ps1 too):   . tests/smoke/drive_3d_smoke.ps1
  then drive the running app one small call at a time, reading the captures back:

    New-Qm3dAssets                  # %TEMP%\qm_3d\cube.obj + cube_atlas.png
    Start-Qa 's1'
    Focus-Qa; Click 42 62; Focus-Qa; Click 120 235      # File > Import 3D Object (OBJ)...
    Submit-QaFileDialog $Qm3dCube 'Import 3D Object'   # types the path into the native dialog
    Cap 's1_import.png'

  The cube is 1 unit, centred on the origin, with v/vt/vn and one atlas cell per
  face (6 coloured, numbered cells, each with a white "UP" bar along its top edge),
  so a texture that comes out upside down or a mirrored face is visible at a glance.
  Click points are physical pixels for the qa_lib window placement (0,0 2560x1360);
  retune them from a capture if the dock layout changes.
#>
. "$PSScriptRoot/qa_lib.ps1"

if (-not ([System.Management.Automation.PSTypeName]'QaFocus').Type) {
    Add-Type @"
using System; using System.Runtime.InteropServices;
public static class QaFocus {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr dlg, int id);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, string l);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
}
"@
}

# Another app on this desktop can grab the foreground between our calls, and then
# our keys land in it. Make $Hwnd the foreground window and prove it, or throw.
function Set-QaForeground([IntPtr]$Hwnd) {
    for ($try = 0; $try -lt 5; $try++) {
        if ([QaFocus]::GetForegroundWindow() -eq $Hwnd) { return }
        $fgThread = [QaFocus]::GetWindowThreadProcessId([QaFocus]::GetForegroundWindow(), [IntPtr]::Zero)
        $me = [QaFocus]::GetCurrentThreadId()
        [QaFocus]::AttachThreadInput($me, $fgThread, $true) | Out-Null
        [QaFocus]::BringWindowToTop($Hwnd) | Out-Null
        [QaFocus]::SetForegroundWindow($Hwnd) | Out-Null
        [QaFocus]::AttachThreadInput($me, $fgThread, $false) | Out-Null
        Start-Sleep -Milliseconds 150
    }
    if ([QaFocus]::GetForegroundWindow() -ne $Hwnd) { throw "could not bring window $Hwnd to the foreground" }
}

# qa_lib's Focus-Qa, but verified (see Set-QaForeground).
function Focus-Qa {
    $hwnd = Get-QaWindow; if ($hwnd -eq [IntPtr]::Zero) { throw "editor window not found" }
    Set-QaForeground $hwnd
    Start-Sleep -Milliseconds 100
}

$Qm3dDir   = Join-Path $env:TEMP 'qm_3d'
$Qm3dCube  = Join-Path $Qm3dDir 'cube.obj'
$Qm3dAtlas = Join-Path $Qm3dDir 'cube_atlas.png'

# Writes the cube OBJ and its 3x2 atlas texture (768x512).
function New-Qm3dAssets {
    New-Item -ItemType Directory -Force -Path $Qm3dDir | Out-Null

    # ---- texture: cell (c, r), r = 0 is the image's top row ----
    $faces = @(
        @{ Name='1 FRONT'; Color=[System.Drawing.Color]::FromArgb(200, 40, 40) },
        @{ Name='2 RIGHT'; Color=[System.Drawing.Color]::FromArgb(40, 170, 60) },
        @{ Name='3 BACK';  Color=[System.Drawing.Color]::FromArgb(40, 70, 200) },
        @{ Name='4 LEFT';  Color=[System.Drawing.Color]::FromArgb(210, 190, 30) },
        @{ Name='5 TOP';   Color=[System.Drawing.Color]::FromArgb(190, 50, 190) },
        @{ Name='6 BOT';   Color=[System.Drawing.Color]::FromArgb(30, 180, 190) })
    $cw = 256; $ch = 256
    $bmp = New-Object System.Drawing.Bitmap(($cw * 3), ($ch * 2))
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $font = New-Object System.Drawing.Font('Arial', 30, [System.Drawing.FontStyle]::Bold)
    $small = New-Object System.Drawing.Font('Arial', 20, [System.Drawing.FontStyle]::Bold)
    for ($i = 0; $i -lt 6; $i++) {
        $x = ($i % 3) * $cw; $y = [math]::Floor($i / 3) * $ch
        $g.FillRectangle((New-Object System.Drawing.SolidBrush($faces[$i].Color)), $x, $y, $cw, $ch)
        $g.FillRectangle([System.Drawing.Brushes]::White, $x, $y, $cw, 44)            # UP bar
        $g.DrawString('^ UP ^', $small, [System.Drawing.Brushes]::Black, $x + 80, $y + 8)
        $g.FillRectangle([System.Drawing.Brushes]::Black, $x, $y + $ch - 20, 40, 20)  # bottom-left tab
        $g.DrawString($faces[$i].Name, $font, [System.Drawing.Brushes]::White, $x + 12, $y + 110)
        $g.DrawRectangle((New-Object System.Drawing.Pen([System.Drawing.Color]::Black, 4)), $x + 2, $y + 2, $cw - 4, $ch - 4)
    }
    $g.Dispose()
    $bmp.Save($Qm3dAtlas, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()

    # ---- mesh: v1..v8 corners; each face BL, BR, TR, TL seen from outside (CCW),
    # with the face's "up" at its TL-TR edge ----
    $h = 0.5
    $v = @("v -$h -$h -$h", "v $h -$h -$h", "v $h $h -$h", "v -$h $h -$h",
           "v -$h -$h $h",  "v $h -$h $h",  "v $h $h $h",  "v -$h $h $h")
    $quads = @(  # corner indices, normal index, atlas cell
        @(5,6,7,8, 1, 0),   # +Z front
        @(6,2,3,7, 2, 1),   # +X right
        @(2,1,4,3, 3, 2),   # -Z back
        @(1,5,8,4, 4, 3),   # -X left
        @(8,7,3,4, 5, 4),   # +Y top (up = -Z)
        @(1,2,6,5, 6, 5))   # -Y bottom (up = +Z)
    $vn = @('vn 0 0 1', 'vn 1 0 0', 'vn 0 0 -1', 'vn -1 0 0', 'vn 0 1 0', 'vn 0 -1 0')
    $vt = @(); $f = @(); $inset = 0.004
    foreach ($q in $quads) {
        $c = $q[5] % 3; $r = [math]::Floor($q[5] / 3)
        $u0 = $c / 3 + $inset; $u1 = ($c + 1) / 3 - $inset
        $vlo = 1 - ($r + 1) / 2 + $inset; $vhi = 1 - $r / 2 - $inset   # OBJ v: 0 = image bottom
        $base = $vt.Count
        $vt += ('vt {0:F4} {1:F4}' -f $u0, $vlo), ('vt {0:F4} {1:F4}' -f $u1, $vlo),
               ('vt {0:F4} {1:F4}' -f $u1, $vhi), ('vt {0:F4} {1:F4}' -f $u0, $vhi)
        $f += 'f ' + ((0..3 | ForEach-Object { '{0}/{1}/{2}' -f $q[$_], ($base + $_ + 1), $q[4] }) -join ' ')
    }
    $obj = @('# QUADmapper 3D smoke cube: 1 unit, atlas cell per face (drive_3d_smoke.ps1)') +
           $v + $vt + $vn + @('g cube') + $f
    Set-Content -Path $Qm3dCube -Value $obj -Encoding ascii
    "wrote $Qm3dCube and $Qm3dAtlas"
}

# Fills the native Open/Save dialog's "File name" box and presses its OK button by
# window messages, so no keystroke can land in another app that grabbed the
# foreground. The dialog opens asynchronously (AsyncFileDialog): wait for it first.
function Submit-QaFileDialog([string]$Path, [string]$Title, [int]$WaitMs = 5000) {
    $deadline = (Get-Date).AddMilliseconds($WaitMs)
    $dlg = [IntPtr]::Zero
    while ($dlg -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 200; $dlg = [QaNative]::FindWindow('#32770', $Title)
    }
    if ($dlg -eq [IntPtr]::Zero) { throw "file dialog '$Title' not found" }
    Start-Sleep -Milliseconds 500                                 # let it finish building
    $combo = [QaFocus]::GetDlgItem($dlg, 0x47C)                   # cmb13: File name
    $edit = if ($combo -ne [IntPtr]::Zero) { [QaFocus]::FindWindowEx($combo, [IntPtr]::Zero, 'Edit', $null) } else { [IntPtr]::Zero }
    if ($edit -eq [IntPtr]::Zero) {                               # Save dialogs: ComboBoxEx32 > ComboBox > Edit
        $inner = [QaFocus]::FindWindowEx($combo, [IntPtr]::Zero, 'ComboBox', $null)
        if ($inner -ne [IntPtr]::Zero) { $edit = [QaFocus]::FindWindowEx($inner, [IntPtr]::Zero, 'Edit', $null) }
    }
    if ($edit -eq [IntPtr]::Zero) { throw "file name box not found in '$Title'" }
    [QaFocus]::SendMessage($edit, 0x000C, [IntPtr]::Zero, $Path) | Out-Null        # WM_SETTEXT
    Start-Sleep -Milliseconds 200
    [QaFocus]::SendMessage($dlg, 0x0111, [IntPtr]1, [IntPtr]::Zero) | Out-Null      # WM_COMMAND IDOK
    $deadline = (Get-Date).AddSeconds(5)
    while ([QaNative]::FindWindow('#32770', $Title) -ne [IntPtr]::Zero -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 200 }
    if ([QaNative]::FindWindow('#32770', $Title) -ne [IntPtr]::Zero) { throw "file dialog '$Title' did not close" }
    Start-Sleep -Milliseconds 500
}

# Keep the editor above other apps' windows so a click at screen coordinates can
# only land in it (its file dialogs are owned by it, so they stay above it).
function Set-QaTopmost {
    [QaFocus]::SetWindowPos((Get-QaWindow), [IntPtr](-1), 0, 0, 0, 0, 0x0003) | Out-Null   # HWND_TOPMOST, NOMOVE|NOSIZE
}
function Click([int]$x, [int]$y, [switch]$Right) {
    Set-QaForeground (Get-QaWindow)
    Move-QaCursor $x $y
    if ($Right) { [QaNative]::mouse_event(0x0008,0,0,0,0); Start-Sleep -Milliseconds 30; [QaNative]::mouse_event(0x0010,0,0,0,0) }
    else        { [QaNative]::mouse_event(0x0002,0,0,0,0); Start-Sleep -Milliseconds 30; [QaNative]::mouse_event(0x0004,0,0,0,0) }
    Start-Sleep -Milliseconds 120
}

# Every key goes to the foreground window: re-assert the editor before each one.
function Tap([int]$vk, [int]$n = 1) {
    for ($i = 0; $i -lt $n; $i++) {
        Set-QaForeground (Get-QaWindow)
        KeyDown $vk; Start-Sleep -Milliseconds 15; KeyUp $vk; Start-Sleep -Milliseconds $QaFrameMs
    }
}
function Chord([int[]]$mods, [int]$vk, [int]$n = 1) {
    Set-QaForeground (Get-QaWindow)
    foreach ($m in $mods) { KeyDown $m; Start-Sleep -Milliseconds 15 }
    for ($i = 0; $i -lt $n; $i++) {
        if ([QaFocus]::GetForegroundWindow() -ne (Get-QaWindow)) {
            foreach ($m in $mods) { KeyUp $m }; throw 'editor lost the foreground mid-chord'
        }
        KeyDown $vk; Start-Sleep -Milliseconds 15; KeyUp $vk; Start-Sleep -Milliseconds $QaFrameMs
    }
    [array]::Reverse($mods); foreach ($m in $mods) { KeyUp $m; Start-Sleep -Milliseconds 15 }
    Start-Sleep -Milliseconds $QaFrameMs
}

# Scroll the mouse wheel at a point (negative = down).
function Wheel([int]$x, [int]$y, [int]$notches) {
    Move-QaCursor $x $y
    $n = [math]::Abs($notches); $d = if ($notches -lt 0) { -120 } else { 120 }
    for ($i = 0; $i -lt $n; $i++) { [QaNative]::mouse_event(0x0800, 0, 0, $d, 0); Start-Sleep -Milliseconds 40 }
    Start-Sleep -Milliseconds 150
}

# Ctrl+click an ImGui drag field to type a value into it.
function Set-QaField([int]$x, [int]$y, [string]$value) {
    Focus-Qa
    KeyDown $VK.CTRL; Start-Sleep -Milliseconds 30; Click $x $y; KeyUp $VK.CTRL
    Paste $value -Enter
}

# Fresh app with the cube imported into the default scene (routed to Projector 1),
# the atlas assigned, the default quad hidden, and the cube turned so three faces
# (seven corners at different depths) face the projector.
function Initialize-Qm3dScene([string]$Name) {
    # A killed run leaves an untitled autosave that opens a "Recover" modal next launch.
    Remove-Item (Join-Path $env:LOCALAPPDATA 'QUADmapper\recovery\untitled.autosave.qmap') -ErrorAction SilentlyContinue
    Start-Qa $Name | Out-Null
    Set-QaTopmost
    Click 52 95                                             # Quad List tab
    Focus-Qa; Click 42 62; Focus-Qa; Move-QaCursor 118 230; Click 120 235
    Submit-QaFileDialog $Qm3dCube 'Import 3D Object'
    Focus-Qa; Click 145 228                                 # hide Quad 1 (it draws on top)
    Click 123 306                                           # select the cube
    Click 521 1143; Click 521 1143                          # Resources > Media
    Click 462 1184                                          # Load Files...
    Submit-QaFileDialog $Qm3dAtlas 'Import Media Files'
    Focus-Qa; Click 2317 598; Start-Sleep -Milliseconds 300 # object Media combo
    Wheel 2300 750 -10; Click 2167 842                      # last item: cube_atlas.png
    Set-QaField 2150 438 '30'; Set-QaField 2252 438 '-35'   # rotation X, Y
    Start-Sleep -Milliseconds 500
}

# Simplified convex hull (screen coords) of the non-background pixels in a rect:
# the silhouette corners of a lone object. $Bg is the background RGB.
function Get-QaSilhouette([int[]]$Rect, [int[]]$Bg = @(0, 0, 0), [int]$Tol = 30, [double]$Eps = 3.0) {
    if (-not ([System.Management.Automation.PSTypeName]'QaHull').Type) {
        Add-Type -ReferencedAssemblies System.Drawing @"
using System; using System.Collections.Generic; using System.Drawing; using System.Linq;
public static class QaHull {
  static double Cross(PointF o, PointF a, PointF b) { return (a.X-o.X)*(b.Y-o.Y)-(a.Y-o.Y)*(b.X-o.X); }
  public static PointF[] Run(Bitmap bmp, int r, int g, int b, int tol, double eps) {
    var pts = new List<PointF>();
    for (int y = 0; y < bmp.Height; y++) { int minx=-1, maxx=-1;
      for (int x = 0; x < bmp.Width; x++) { var c = bmp.GetPixel(x,y);
        if (Math.Abs(c.R-r)+Math.Abs(c.G-g)+Math.Abs(c.B-b) > tol) { if (minx<0) minx=x; maxx=x; } }
      if (minx>=0) { pts.Add(new PointF(minx,y)); pts.Add(new PointF(maxx,y)); } }
    var p = pts.OrderBy(q=>q.X).ThenBy(q=>q.Y).ToList(); var h = new List<PointF>();
    foreach (var q in p) { while (h.Count>=2 && Cross(h[h.Count-2],h[h.Count-1],q)<=0) h.RemoveAt(h.Count-1); h.Add(q); }
    int lo = h.Count+1;
    for (int i=p.Count-2;i>=0;i--) { var q=p[i]; while (h.Count>=lo && Cross(h[h.Count-2],h[h.Count-1],q)<=0) h.RemoveAt(h.Count-1); h.Add(q); }
    h.RemoveAt(h.Count-1);
    while (h.Count > 3) { int best=-1; double bd=1e9;
      for (int i=0;i<h.Count;i++) { var a=h[(i+h.Count-1)%h.Count]; var c=h[(i+1)%h.Count]; var m=h[i];
        double len=Math.Sqrt((c.X-a.X)*(c.X-a.X)+(c.Y-a.Y)*(c.Y-a.Y)); double d=Math.Abs(Cross(a,c,m))/Math.Max(len,1e-6);
        if (d<bd) { bd=d; best=i; } }
      if (bd>=eps) break; h.RemoveAt(best); }
    return h.ToArray();
  }
}
"@
    }
    $path = Join-Path $QaOut '_hull.png'
    Save-ScreenRect (New-Object System.Drawing.Rectangle($Rect[0], $Rect[1], $Rect[2], $Rect[3])) $path
    $bmp = [System.Drawing.Bitmap]::FromFile($path)
    try { [QaHull]::Run($bmp, $Bg[0], $Bg[1], $Bg[2], $Tol, $Eps) | ForEach-Object { ,@([int]($_.X + $Rect[0]), [int]($_.Y + $Rect[1])) } }
    finally { $bmp.Dispose() }
}

# The Calibration panel's model view: its top edge moves as the point table grows,
# so find it (first row of the view's clear colour) before measuring the model.
$QaModelBg = @(28, 28, 33)
function Get-QaModelSilhouette {
    $path = Join-Path $QaOut '_col.png'
    Save-ScreenRect (New-Object System.Drawing.Rectangle(2115, 500, 1, 840)) $path
    $bmp = [System.Drawing.Bitmap]::FromFile($path); $top = -1
    try { for ($y = 0; $y -lt $bmp.Height; $y++) { $c = $bmp.GetPixel(0, $y)
            if ([math]::Abs($c.R - 28) + [math]::Abs($c.G - 28) + [math]::Abs($c.B - 33) -le 3) { $top = 500 + $y; break } } }
    finally { $bmp.Dispose() }
    if ($top -lt 0) { throw 'model view not found' }
    Get-QaSilhouette @(2110, ($top + 4), 420, (1330 - $top - 4)) $QaModelBg
}

# Grep the app's stdout log for a pattern (the logger writes there).
function Get-QaLog([string]$Name, [string]$Pattern = '.') {
    Select-String -Path (Join-Path $QaOut "${Name}_stdout.log") -Pattern $Pattern | ForEach-Object { $_.Line }
}
