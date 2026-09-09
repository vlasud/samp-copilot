#pragma once
//
// What he knows because he has walked it.
//
// SA-MP's own answer for getting about inside a server's custom interiors is
// the GPS plugin: nodes and connections placed by hand, and A* over them.
// Nobody derives passability from the geometry, because the geometry lies -
// a glass wall built out of a door's model, a railing thinner than any probe,
// a doorway sealed by its own clearance. The game's own path nodes cover the
// outdoors and stop at every threshold.
//
// So the nodes are placed here too, only nobody places them: a square the
// character has physically stood in is passable, and no map can argue with
// it. A step from one square to the next is a connection, and it stays a
// connection whatever was in the way - a door he pushed open, a gap between
// two beds his shoulders turned through. The graph is exactly the ground
// truth of a place, gathered by being in it.
//
// It is written to disk beside the module, so a hospital is learned once.
//
#include <cstdint>
#include <string>
#include <vector>

#include "game/world_query.hpp"

namespace gtabot::nav {

using game::Vec3;

// Game thread, every frame while he is on his feet. Records the square he is
// standing in, and the step from the last one.
void TrailVisit(const Vec3& at, bool on_ground);

// A way from here to there over squares he has walked, or empty when the
// graph does not join them. Points are a metre or so apart, in order.
std::vector<Vec3> TrailRoute(const Vec3& from, const Vec3& to);

// The nearest square on record to a point, and how far off it is. Used to
// tell "I have been near there" from "I have never been anywhere near".
bool TrailNearest(const Vec3& to, Vec3* at, float* away_m);

// Reads and writes bot.trail beside the module. Loading is done once, on the
// first visit; saving happens by itself every so often and on the way out.
void TrailSave();
void TrailLoad();

struct TrailFacts {
  std::size_t squares = 0;
  std::size_t steps = 0;
  int         routes_found = 0;
  int         routes_missed = 0;
  bool        loaded = false;
  std::string note;
};
TrailFacts TrailGet();

}  // namespace gtabot::nav
