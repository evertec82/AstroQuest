# Keeps pictures of a window as it changes: so many a second for so many seconds, as JPEGs named
# by the milliseconds since the start. For looking afterwards at what happened in a game that
# somebody plays in a headset, whose window on the desktop shows what both eyes see.
#   powershell -File tools/window-record.ps1 <title regex> <folder> [seconds] [pictures a second]
param([Parameter(Mandatory = $true)][string]$Title, [Parameter(Mandatory = $true)][string]$Folder,
      [double]$Seconds = 20, [double]$Rate = 6)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class Shown {
    public delegate bool EnumProc(IntPtr hwnd, IntPtr lparam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, IntPtr lparam);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowText(IntPtr hwnd, StringBuilder text, int count);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    public static IntPtr Find(string pattern) {
        IntPtr found = IntPtr.Zero;
        var regex = new System.Text.RegularExpressions.Regex(pattern);
        EnumWindows((h, l) => {
            if (!IsWindowVisible(h)) return true;
            var text = new StringBuilder(512);
            GetWindowText(h, text, 512);
            if (regex.IsMatch(text.ToString())) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
"@

$window = [Shown]::Find($Title)
if ($window -eq [IntPtr]::Zero) { Write-Output "no window matches '$Title'"; exit 1 }
[void][System.IO.Directory]::CreateDirectory($Folder)
$client = New-Object Shown+RECT
[void][Shown]::GetClientRect($window, [ref]$client)
$width = [Math]::Max(1, $client.Right)
$height = [Math]::Max(1, $client.Bottom)
$bitmap = New-Object System.Drawing.Bitmap $width, $height
$codec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq "image/jpeg" }
$quality = New-Object System.Drawing.Imaging.EncoderParameters 1
$quality.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter ([System.Drawing.Imaging.Encoder]::Quality), 88L
$clock = [System.Diagnostics.Stopwatch]::StartNew()
$taken = 0
while ($clock.Elapsed.TotalSeconds -lt $Seconds) {
    $at = $clock.ElapsedMilliseconds
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $hdc = $graphics.GetHdc()
    # 1 = client area only, 2 = also what the window draws with the GPU
    [void][Shown]::PrintWindow($window, $hdc, 3)
    $graphics.ReleaseHdc($hdc)
    $graphics.Dispose()
    $bitmap.Save((Join-Path $Folder ("{0:d6}.jpg" -f $at)), $codec, $quality)
    $taken++
    $next = $taken * 1000.0 / $Rate
    $wait = $next - $clock.ElapsedMilliseconds
    if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]$wait) }
}
Write-Output ("{0} pictures of {1}x{2} in {3:N1} s -> {4}" -f $taken, $width, $height, $clock.Elapsed.TotalSeconds, $Folder)
