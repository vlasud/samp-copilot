# One alt-tab, so the game leaves exclusive fullscreen.
#
# The game always starts fullscreen: the mod's windowed mode only takes at
# the first device reset, and a reset is what minimising and restoring
# causes. Until that happens any focus change - a screenshot, a script
# bringing the window forward, the user's own alt-tab - can leave the game
# holding a lost device and rendering nothing at all, which reads in every
# test as a character who will not move. Doing it once on purpose, right
# after the launch, costs a second and saves the evening.
param([string]$Process = "gta_sa")
Add-Type @"
using System;using System.Runtime.InteropServices;
public class Win {
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
}
"@
$p = Get-Process -Name $Process -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if ($null -eq $p) { Write-Output "no window for $Process"; exit 1 }
$h = $p.MainWindowHandle
[void][Win]::ShowWindow($h, 6)     # SW_MINIMIZE
Start-Sleep -Milliseconds 1200
[void][Win]::ShowWindow($h, 9)     # SW_RESTORE
Start-Sleep -Milliseconds 1200
[void][Win]::SetForegroundWindow($h)
Write-Output ("minimised and restored; iconic now: {0}" -f [Win]::IsIconic($h))
