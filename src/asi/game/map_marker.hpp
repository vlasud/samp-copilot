#pragma once
//
// The waypoint on the map.
//
// A player puts a marker on the pause-menu map with one click, and the game
// keeps it as an ordinary coordinate blip whose handle sits in the front-end
// menu manager. That is the most natural way there is to tell the character
// where to go - no numbers to type - so the panel offers "go to the marker"
// and this is what reads it. Read-only: the handle, and the blip it names.
//
#include "game/world_query.hpp"

namespace gtabot::game {

// True, with the marker's position, when there is a marker on the map. Its
// height is whatever the map gave it - usually nothing - so a journey to it
// finds the ground itself.
bool MapMarker(Vec3* out);

}  // namespace gtabot::game
