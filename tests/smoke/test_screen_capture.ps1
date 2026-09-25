<#
.SYNOPSIS
  Regression check for screen_capture.ps1 (QM-096): a screen pixel of RGB(13,11,12),
  the GDI+ GetHdc marker colour, must be captured as itself, opaque.

.DESCRIPTION
  Shows a borderless window filled with RGB(13,11,12), captures its centre with
  Save-ScreenRect and checks the pixel. Graphics.CopyFromScreen returns it as
  transparent black, which is how dark video looked speckled in QA captures.
  Needs an interactive desktop. Exit code 0 = pass, 1 = fail.
#>
Add-Type -AssemblyName System.Windows.Forms
. "$PSScriptRoot/screen_capture.ps1"
if (-not ([System.Management.Automation.PSTypeName]'CapTestDpi').Type) {
    Add-Type 'using System.Runtime.InteropServices; public static class CapTestDpi { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); }'
}
[CapTestDpi]::SetProcessDPIAware() | Out-Null

$marker = [System.Drawing.Color]::FromArgb(255, 13, 11, 12)
$form = New-Object System.Windows.Forms.Form
$form.FormBorderStyle = 'None'
$form.StartPosition = 'Manual'
$form.Bounds = New-Object System.Drawing.Rectangle(200, 200, 200, 200)
$form.BackColor = $marker
$form.TopMost = $true
$form.Show()
$form.Refresh()
Start-Sleep -Milliseconds 700

$path = Join-Path ([System.IO.Path]::GetTempPath()) 'qm096_capture_check.png'
try {
    Save-ScreenRect (New-Object System.Drawing.Rectangle(250, 250, 100, 100)) $path
} finally {
    $form.Close(); $form.Dispose()
}

$bmp = New-Object System.Drawing.Bitmap($path)
$px = $bmp.GetPixel(50, 50)
$bmp.Dispose()
Remove-Item $path -ErrorAction SilentlyContinue

if ($px.A -eq 255 -and $px.R -eq 13 -and $px.G -eq 11 -and $px.B -eq 12) {
    "PASS: captured $px"
    exit 0
}
"FAIL: expected opaque RGB(13,11,12), captured $px"
exit 1
