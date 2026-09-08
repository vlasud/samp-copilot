#pragma once
//
// Who writes there?
//
// Two places decide whether the player has any input at all, and both are
// written by code that does not say so: the five bytes at 0x541DF5 in
// gta_sa.exe (the call to AffectPadFromKeyBoard, which SA-MP replaces with
// NOPs to switch the keyboard off) and the dword at samp.dll+0x21A130 (above
// ten, SA-MP's window procedure returns without passing a single message
// on). Polling shows them change; it cannot show whose instruction did it.
//
// A hardware write watchpoint can. The debug registers of every thread in
// the process are set to trap a write to those addresses, and the vectored
// handler that catches the trap logs the instruction that just wrote, the
// module it belongs to, the value now there, and the return addresses on
// the stack. Nothing is changed and nothing is prevented: the write has
// already happened when the trap fires, and execution continues.
//
#include <cstdint>
#include <string>

namespace gtabot::game {

// Sets the watchpoints on every thread of the process, and again on threads
// that appear later. Safe to call every tick; does nothing once done for
// all current threads.
void WatchpointsInstall();

// Hits so far, for the input line.
std::string WatchpointLine();

}  // namespace gtabot::game
