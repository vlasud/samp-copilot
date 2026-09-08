# Brings the game's window to the front.
#
# Keystrokes go to whichever window has the focus, so anything the mod types
# - a password into the server's dialog, a key a prompt asked for - lands
# somewhere else entirely when a terminal is in front. Taking a screenshot
# from here is enough to lose it, so this puts it back.
param([string]$Process = "gta_sa")
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Fg {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr p);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
}
"@
$p = Get-Process -Name $Process -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if ($null -eq $p) { Write-Output "no window for $Process"; exit 1 }
$h = $p.MainWindowHandle
# Windows only lets the foreground thread hand the focus on, so borrow its
# input queue for the moment it takes to do that.
$other = [Fg]::GetWindowThreadProcessId([Fg]::GetForegroundWindow(), [IntPtr]::Zero)
$mine = [Fg]::GetCurrentThreadId()
[void][Fg]::AttachThreadInput($mine, $other, $true)
[void][Fg]::ShowWindow($h, 9)
[void][Fg]::SetForegroundWindow($h)
[void][Fg]::AttachThreadInput($mine, $other, $false)
Start-Sleep -Milliseconds 400
if ([Fg]::GetForegroundWindow() -eq $h) { Write-Output "the game is in front" }
else { Write-Output "the game did not come to the front" }
