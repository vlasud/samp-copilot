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
