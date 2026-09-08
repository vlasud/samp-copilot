# Lets go of any key the system still thinks is held.
#
# Keys are pressed through SendInput, which sets the state for the whole
# session, not just the game. Kill the game between a key going down and the
# same key coming up - a crash, a forced close, a frozen frame - and Windows
# goes on believing a hand is holding it. A stuck Alt is the worst of them:
# every window then behaves as though its menu is being opened, and the next
# game to start never renders a frame.
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Keys {
  [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vk);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
}
"@
$watch = @{
  0x10 = "Shift"; 0xA0 = "LShift"; 0xA1 = "RShift"
  0x11 = "Ctrl";  0xA2 = "LCtrl";  0xA3 = "RCtrl"
  0x12 = "Alt";   0xA4 = "LAlt";   0xA5 = "RAlt"
  0x57 = "W"; 0x41 = "A"; 0x53 = "S"; 0x44 = "D"
  0x20 = "Space"; 0x0D = "Enter"; 0x09 = "Tab"; 0x43 = "C"
  0x26 = "Up"; 0x28 = "Down"; 0x25 = "Left"; 0x27 = "Right"
}
$freed = @()
foreach ($vk in $watch.Keys) {
  if (([Keys]::GetAsyncKeyState($vk) -band 0x8000) -ne 0) {
    $scan = [byte][Keys]::MapVirtualKey([uint32]$vk, 0)
    [Keys]::keybd_event([byte]$vk, $scan, 0x0002, [IntPtr]::Zero)   # KEYEVENTF_KEYUP
    Start-Sleep -Milliseconds 30
    $freed += $watch[$vk]
  }
}
if ($freed.Count -eq 0) { Write-Output "nothing was held" }
else { Write-Output ("let go of: " + ($freed -join ", ")) }
