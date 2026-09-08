# Switches the game window's keyboard layout.
#
# Under a Russian layout the game does not see letter keys at all: T does not
# open the chat, and the walk's own W, A, S and D are read as nothing. The
# game asks Windows what character a key produces and gets a Cyrillic one,
# which matches none of the letters its code compares against. Switching the
# window to English is what a player does without thinking about it.
param([string]$Lang = "en")
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Lay {
  [DllImport("user32.dll")] public static extern IntPtr LoadKeyboardLayout(string id, uint flags);
  [DllImport("user32.dll")] public static extern IntPtr PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetKeyboardLayout(uint threadId);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr p);
}
"@
$id = if ($Lang -eq "ru") { "00000419" } else { "00000409" }
$hkl = [Lay]::LoadKeyboardLayout($id, 0x00000101)   # ACTIVATE | SUBSTITUTE_OK
$p = Get-Process -Name gta_sa -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if ($null -eq $p) { Write-Output "no game window"; exit 1 }
# WM_INPUTLANGCHANGEREQUEST, with the flag that says "this handle, please".
[void][Lay]::PostMessage($p.MainWindowHandle, 0x0050, [IntPtr]1, $hkl)
Start-Sleep -Milliseconds 300
$thread = [Lay]::GetWindowThreadProcessId($p.MainWindowHandle, [IntPtr]::Zero)
$now = [Lay]::GetKeyboardLayout($thread)
Write-Output ("the game window is now on layout 0x{0:X}" -f ($now.ToInt64() -band 0xFFFF))
