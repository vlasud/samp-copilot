#pragma once
//
// Reads every stage a key passes through on its way to the character, so a
// character that will not move can be blamed on the right stage in one
// reading rather than argued about.
//
// The stages, in order, and where each is read from:
//
//   Windows      GetAsyncKeyState - the key is physically down
//   the window   GetGUIThreadInfo - which window the game thread's keyboard
//                focus is on, and the game's own ForegroundApp flag, which
//                WM_KILLFOCUS clears and WM_SETFOCUS sets
//   WM_KEYDOWN   CPad::TempKeyState - written by the game's window procedure
//   UpdatePads   CPad::NewKeyState - copied from TempKeyState once a frame
//   the pad      CPad::Pads[0].NewState - what AffectPadFromKeyBoard made of
//                it, which is what the character is steered by
//   the ped      flags, state, speed, and the tasks running on him
//
// Everything is a validated read of memory. Nothing here calls into the
// game, so it is safe from any thread, and it is meant for the worker thread
// - the one that keeps running when the input has gone.
//
#include <cstdint>
#include <string>

namespace gtabot::game {

// The short form, for the input line that is logged whenever it changes.
std::string InputPipelineBrief(std::uintptr_t game_ped, void* game_window);

// Everything, for the moment the input is found to be gone.
std::string InputPipelineFull(std::uintptr_t game_ped, void* game_window);

// The first bytes, in memory, of the functions that carry a key to the
// character and of the three this module calls - against what the file on
// disk has there. A hook placed by another module shows up as a jump out of
// the executable, and is named by the module it jumps into.
std::string CodeIntegrityReport();

// How many threads this process has right now.
int ThreadCount();

}  // namespace gtabot::game
