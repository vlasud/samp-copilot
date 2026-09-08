#pragma once
//
// Diagnostics that answer one question: are we actually reading SA-MP's
// memory, or only claiming to?
//
// Everything here runs on the game thread, inside the frame hook. That is what
// makes a full scan safe - the thread that allocates and frees is the one
// executing us, so nothing can be unmapped mid-read.
//
#include "types.hpp"

namespace gtabot::asi {

// Game thread only. Cheap; built every few frames into the bridge's world
// slot. This is where players and vehicles will land.
json BuildWorldSnapshot();

// Where the local player was when the snapshot was last built, published for
// threads that must not walk SA-MP's structures themselves. Returns false
// until there has been one.
bool LastLocalPosition(float* x, float* y, float* z);

// How many world snapshots have been built. A position that has not been
// refreshed cannot be used to say the player has stopped: when reading him
// starts failing, the last one read stays put and looks exactly like a man
// who is not moving.
unsigned LastPositionSerial();

// The game's own object for the local player, as of the last snapshot, for
// threads that may read his fields but must not walk SA-MP's structures to
// find him. Zero until there has been one.
std::uintptr_t LastLocalPedPointer();

// Worker thread. Touches only its own counters and code bytes in d3d9.dll, so
// it keeps reporting while the game thread is stalled - which is exactly when
// the report matters most.
json BuildStatusSnapshot();

// Expensive and explicit. Costs a visible frame hitch when it sweeps the whole
// process, which is why it only ever runs when asked for.
json ProbeMemory(const json& args);

// Writes the outcome of a probe to the log in a couple of lines. The log is
// the one place a result survives being alt-tabbed away from.
void LogProbeSummary(const json& result);

}  // namespace gtabot::asi
