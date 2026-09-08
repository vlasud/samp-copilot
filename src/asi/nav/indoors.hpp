#pragma once
//
// A map of the room he is standing in, drawn by feeling the walls.
//
// Nothing the planner knows works indoors. The game's pedestrian graph stops
// at the door of every building, and the open-ground search asks whether a
// place can be stood in - a clear line from the floor to head height - which
// a low ceiling refuses everywhere, including under the character's own
// feet. A room built out of a server's objects is invisible to both.
//
// So it is felt instead: a grid over the floor around him, and between two
// neighbouring squares a pair of horizontal lines at knee and chest height.
// A wall stops both. A doorway stops neither. Flood the grid from where he
// stands and what comes back is the room - and where the room ends nearest
// to wherever he was trying to go is the door, whether it is open or shut.
//
#include <string>
#include <vector>

#include "game/world_query.hpp"

namespace gtabot::nav {

using game::Vec3;

struct Room {
  bool  ok = false;              // the flood ran
  std::string note;
  int   cells_reached = 0;
  float cell_m = 0;
  // A way from where he stands to the target, when the target is inside the
  // room; otherwise as far towards it as the room goes.
  std::vector<Vec3> points;
  bool  reaches_target = false;
  // Where the room ends nearest the target: the way out, open or shut.
  bool  way_out_found = false;
  Vec3  way_out;
  float way_out_away_m = 0;
  // The picture, one character a square, for a person or an agent to read.
  std::vector<std::string> picture;
};

// Game thread: this reads the world a great many times. `radius` is how far
// around him to feel, in metres.
Room MapRoom(const Vec3& from, const Vec3& towards, float radius);

}  // namespace gtabot::nav
