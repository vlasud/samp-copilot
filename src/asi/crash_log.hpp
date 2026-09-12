#pragma once
//
// Turns "and the game crashed" into an address and a module name.
//
// Installs an unhandled-exception filter that writes the fault to
// bot.asi.log and then hands control to whatever filter was already there, so
// the game's own crash handling is left intact.
//
namespace gtabot::asi {

void InstallCrashLogger();

// Says who ended the session, which an exception filter cannot: most of the
// times this client has disappeared it left no crash at all, because nothing
// faulted - somebody asked the process to close. Hooks the two ways that can
// happen from inside and names the caller.
//
// The absence of either line is evidence too: a session that ends with neither
// a crash nor an exit line was killed from outside the process, where nothing
// of ours gets to run.
//
// Needs MinHook, so it goes in after the frame hook.
void WatchProcessExit();

}  // namespace gtabot::asi
