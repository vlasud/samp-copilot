param([string]$out)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  public struct RECT { public int L, T, R, B; }
}
"@
$h = [W]::FindWindow("Grand theft auto San Andreas", $null)
if ($h -eq [IntPtr]::Zero) { $h = (Get-Process gta_sa -ErrorAction SilentlyContinue | Select-Object -First 1).MainWindowHandle }
$r = New-Object W+RECT
[void][W]::GetWindowRect($h, [ref]$r)
$w = $r.R - $r.L; $ht = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $ht
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
$small = New-Object System.Drawing.Bitmap $bmp, ([int]($w*0.5)), ([int]($ht*0.5))
$small.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Output ("{0} {1}x{2}" -f $out, $small.Width, $small.Height)
