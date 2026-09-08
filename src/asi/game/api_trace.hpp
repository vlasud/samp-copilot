#pragma once
//
// What SA-MP asks Windows just before it acts.
//
// The protection inside samp.dll decides to switch the game's input off in
// a frame, and nothing in the game's memory says why. What it consulted in
// that frame it consulted through Windows: key states, window handles,
// timers, memory queries, thread contexts, module lists. Those calls can be
// seen. A set of such functions is hooked and every call whose return
// address lies inside samp.dll is written into a ring buffer - the function,
// two arguments, the caller. When the watchpoint sees the gate written, the
// last two seconds of that buffer are dumped: the protection's questions,
// in order, ending with its verdict.
//
#include <cstdint>
#include <string>

namespace gtabot::game {

// Hooks the functions. MinHook must be initialised; safe to call every tick.
bool ApiTraceInstall();

// Any thread: asks for the buffer to be dumped by the worker, with a reason.
void ApiTraceRequestDump(const char* why);

// Worker thread: writes the dump if one was asked for.
void ApiTraceDumpIfAsked();

// For the input line: entries recorded so far.
std::string ApiTraceLine();

}  // namespace gtabot::game
