# A picture of the game window, for looking at what the numbers cannot say.
#
# The mod reports positions, models and text; none of that shows a character
# standing the wrong side of a counter, or a dialog drawn over half the
# screen. This grabs the window as the player sees it.
param(
  [string]$Path = "$env:TEMP\gtabot-shot.png",
  [string]$Process = "gta_sa"
)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@
$p = Get-Process -Name $Process -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if ($null -eq $p) { Write-Output "no window for $Process"; exit 1 }
if ([Win]::IsIconic($p.MainWindowHandle)) { Write-Output "the window is minimised"; exit 1 }
$r = New-Object Win+RECT
[void][Win]::GetWindowRect($p.MainWindowHandle, [ref]$r)
$w = $r.R - $r.L; $h = $r.B - $r.T
if ($w -le 0 -or $h -le 0) { Write-Output "the window has no size"; exit 1 }
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
$g.Dispose()
$bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output "$Path ($w x $h)"
