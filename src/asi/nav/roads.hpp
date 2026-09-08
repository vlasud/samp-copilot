#pragma once
//
// A route along the roads, for something with wheels.
//
// The graph the game keeps has two halves. The pavements are what the
// planner walks, and they are hard work: the game's pedestrians cut across
// squares and through gates, so every leg has to be checked against the
// world before it is believed. The roads are the other half, and they are
// nothing like as hard. Traffic drives them all day, they join where cars
// can turn, and a car that follows them is on a road by construction - so
// this is a search over the graph and no questions asked of the world at
// all.
//
// What comes back is a chain of points down the middle of the roads. Turning
// that into a driven line is the driver's problem, not this one's.
//
#include <string>
#include <vector>

#include "game/world_query.hpp"

namespace gtabot::nav {

using game::Vec3;

struct Road {
  bool ok = false;
  std::string note;
  std::vector<Vec3> points;   // from near `from` to near `to`
  float length_m = 0;
  int   nodes_looked_at = 0;
};

// Game thread. Empty when the graph has not resolved or the two ends cannot
// be joined to it - which, out in the country or inside an interior, is an
// honest answer rather than a fault.
Road RoadRoute(const Vec3& from, const Vec3& to);

}  // namespace gtabot::nav
