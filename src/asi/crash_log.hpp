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

}  // namespace gtabot::asi
