#pragma once
//
// Where the process's threads actually are.
//
// "The game is not rendering" is a symptom with several very different
// causes: a wait on a lock nobody will release, a loop in our own code, a
// call into the driver that never comes back, the game's own pause. From
// outside they look identical - frames stop - and guessing between them
// costs a restart each time.
//
// This asks. Every thread of the process is suspended for the moment it
// takes to read its instruction pointer, and the answer says which module
// that address belongs to. A game thread sitting in bot.asi is our bug; one
// sitting in d3d9 or the kernel is a wait; one in gta_sa is the game's own.
//
#include <string>

namespace gtabot::game {

// One line per thread: its id, where it is, and whose code that is. Must be
// called from a thread other than the one being asked about - it suspends
// each in turn - so: the module's own watchdog thread, never the game's.
std::string WhereThreadsAre();

}  // namespace gtabot::game
