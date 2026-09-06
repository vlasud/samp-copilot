#pragma once
//
// Diagnostics that answer one question: are we actually reading SA-MP's
// memory, or only claiming to?
//
// Everything here runs on the game thread, inside the frame hook. That is what
// makes a full scan safe - the thread that allocates and frees is the one
// executing us, so nothing can be unmapped mid-read.
//
#include "common/protocol.hpp"

namespace gtabot::asi {

// Cheap; built every few frames and shipped as the periodic snapshot.
proto::json BuildSnapshot();

// Expensive and explicit. Costs a visible frame hitch when it sweeps the whole
// process, which is why it only ever runs when asked for.
proto::json ProbeMemory(const proto::json& args);

}  // namespace gtabot::asi
