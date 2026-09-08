#pragma once
//
// Getting somewhere, as opposed to walking a route.
//
// The walker follows a list of points and stops when it runs out of them, or
// when something is in the way. That is enough to cross a street and not
// enough to cross a city, for two reasons the planner cannot do anything
// about on its own:
//
//   - The game only knows the few hundred metres it has streamed in. A point
//     across town has no ground under it yet, so no route can be planned to
//     it - until you walk far enough that it does.
//   - A route is planned against the world as it was. Cars move, gates close,
//     people stand in doorways. A leg that was walkable a minute ago need not
//     be walkable now.
//
// So this keeps the destination rather than the route. It plans as far toward
// it as the loaded world allows, walks that, and plans again from wherever it
// ends up - on arrival because more of the city has appeared, and on being
// blocked because the thing in the way may not be there any more. It gives up
// only when several attempts in a row make no progress, and says so.
//
#include <string>

#include "game/world_query.hpp"

namespace gtabot::act {

using game::Vec3;

struct TravelStatus {
  bool        travelling = false;
  Vec3        destination;
  float       straight_m = 0;   // as the crow flies, from here
  int         replans    = 0;
  int         failures   = 0;   // attempts in a row that got nowhere
  bool        reaching   = false;  // heading for a staging point, not the goal
  std::string note;
};

// Go there. Replaces any travel or walk already running. A destination whose
// height is not known - a marker on the map has none - says so, and the
// ground under it is found once the journey is near enough to ask.
void TravelTo(const Vec3& destination, bool height_unknown = false);
void CancelTravel(const char* why);

// Game thread, once a frame. Cheap while the walker is busy; plans only when
// it is not.
void TravelTick();

TravelStatus TravelGet();

}  // namespace gtabot::act
