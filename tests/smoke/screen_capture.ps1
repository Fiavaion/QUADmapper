<#
.SYNOPSIS
  Screen capture for the smoke/QA harnesses that keeps every pixel's true colour.

.DESCRIPTION
  Graphics.CopyFromScreen is not safe for pixel checks: GDI+ pre-fills the bitmap
  behind Graphics.GetHdc with the marker colour RGB(13,11,12) and, on ReleaseHdc,
  treats every pixel still holding that colour as untouched. A screen pixel that is
  genuinely RGB(13,11,12) therefore comes back transparent (ARGB) or black (RGB).
  Dark video is full of that colour, which QM-096 misread as white specks (the PNG
  viewer showed the transparent pixels over white). This BitBlts with plain GDI and
  converts the HBITMAP afterwards, so no marker colour is involved.

  Dot-source it, then:  Save-ScreenRect $bounds $path   ($bounds: System.Drawing.Rectangle)
#>
Add-Type -AssemblyName System.Drawing
if (-not ([System.Management.Automation.PSTypeName]'ScreenGdi').Type) {
    Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class ScreenGdi {
    [DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr hWnd, IntPtr hDC);
    [DllImport("gdi32.dll")] public static extern IntPtr CreateCompatibleDC(IntPtr hDC);
    [DllImport("gdi32.dll")] public static extern IntPtr CreateCompatibleBitmap(IntPtr hDC, int w, int h);
    [DllImport("gdi32.dll")] public static extern IntPtr SelectObject(IntPtr hDC, IntPtr obj);
    [DllImport("gdi32.dll")] public static extern bool BitBlt(IntPtr dst, int x, int y, int w, int h, IntPtr src, int sx, int sy, uint rop);
    [DllImport("gdi32.dll")] public static extern bool DeleteObject(IntPtr obj);
    [DllImport("gdi32.dll")] public static extern bool DeleteDC(IntPtr hDC);
}
"@
}

function Save-ScreenRect([System.Drawing.Rectangle]$Bounds, [string]$Path) {
    $screen = [ScreenGdi]::GetDC([IntPtr]::Zero)
    $mem = [ScreenGdi]::CreateCompatibleDC($screen)
    $hbm = [ScreenGdi]::CreateCompatibleBitmap($screen, $Bounds.Width, $Bounds.Height)
    $old = [ScreenGdi]::SelectObject($mem, $hbm)
    try {
        # SRCCOPY | CAPTUREBLT (include layered windows, as CopyFromScreen did)
        if (-not [ScreenGdi]::BitBlt($mem, 0, 0, $Bounds.Width, $Bounds.Height,
                                     $screen, $Bounds.X, $Bounds.Y, 0x40CC0020)) {
            throw "BitBlt failed capturing $Bounds"
        }
        [ScreenGdi]::SelectObject($mem, $old) | Out-Null
        $bmp = [System.Drawing.Image]::FromHbitmap($hbm)
        try { $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png) } finally { $bmp.Dispose() }
    } finally {
        [ScreenGdi]::DeleteDC($mem) | Out-Null
        [ScreenGdi]::DeleteObject($hbm) | Out-Null
        [ScreenGdi]::ReleaseDC([IntPtr]::Zero, $screen) | Out-Null
    }
}
