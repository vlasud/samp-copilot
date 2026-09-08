#pragma once
//
// What has happened since you last looked.
//
// Everything else here answers "how are things now", and something driving
// this module from outside would have to ask all of it, over and over, to
// notice that a dialog opened for a second or that a line of chat named it.
// This is the other half: a short memory of things that happened, each with
// a number, so one call catches up on all of them and says where to carry on
// from.
//
// It is deliberately small - a few hundred entries - and it drops the oldest
// when it fills, saying how many it dropped rather than pretending.
//
#include <string>
#include <vector>

#include "types.hpp"

namespace gtabot::state {

// Anything can note something worth reacting to. Thread-safe.
void Note(const std::string& kind, const std::string& text, json extra = json::object());

// Game thread, a few times a second: notices what changed - dialogs, the
// chat, spawning and dying, the end of a journey - and notes it.
void WatchEvents();

// Everything after `since`, oldest first, with the cursor to use next time.
// A `since` of zero means "whatever you still hold".
json EventsSince(long long since, int limit,
                 const std::vector<std::string>& kinds = {});

}  // namespace gtabot::state
