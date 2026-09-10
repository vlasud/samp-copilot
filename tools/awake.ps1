# Keeps the display and the system awake while a test runs.
#
# A sleeping display stops the compositor, and the game's render loop stops
# with it: every thread parks in ntdll and the mod sees no frames at all,
# which reads in a test as a character who will not move. Measured at six in
# the morning after several hours without a human touching the machine.
param([int]$Minutes = 180)
Add-Type @"
using System;using System.Runtime.InteropServices;
public class Awake {
  [DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint f);
}
"@
$CONTINUOUS = 0x80000000; $SYSTEM = 0x00000001; $DISPLAY = 0x00000002
[void][Awake]::SetThreadExecutionState($CONTINUOUS -bor $SYSTEM -bor $DISPLAY)
Write-Output "awake for $Minutes minutes"
$until = (Get-Date).AddMinutes($Minutes)
while ((Get-Date) -lt $until) {
  [void][Awake]::SetThreadExecutionState($CONTINUOUS -bor $SYSTEM -bor $DISPLAY)
  Start-Sleep -Seconds 30
}
[void][Awake]::SetThreadExecutionState($CONTINUOUS)
