#pragma once
//
// The walkable ground between here and there, drawn from the world's own
// collision - one map for the street and the room alike.
//
// What everybody who has solved this for a game has ended up doing: not
// asking the world a question here and a question there, but rasterising
// its solid geometry onto a field, working out where a body fits, and
// searching that. Recast does it in three dimensions with voxels; the
// pavement graph the game ships stops at every door and knows nothing a
// server has built since. This does it on a flat grid, half a metre a cell,
// with the ground under each cell read separately so a kerb is a step and a
// wall is a wall.
//
// Three things fall out of that which the old way could not give:
//
//   clearance - every cell knows how far the nearest wall is, and walking
//   costs more near one, so the route keeps to the middle of a pavement by
//   itself instead of being steered off the bricks after touching them;
//
//   one algorithm - the room the character stands in is painted the same
//   way (indoors.cpp), and a route that passes through a door does not
//   change planners at the threshold;
//
//   honesty at the edge - only what is streamed in has collision, and where
//   nothing is streamed the ground read fails and the cell is closed. The
//   field ends where the world does, and says so, rather than drawing a
//   road through a building the game has not loaded yet.
//
// The work is split into units small enough to sit beside a frame - a tile
// of paint, a batch of ground reads, a few thousand expansions - and a
// caller steps it until it says it is done.
//
#include <cstdint>
#include <string>
#include <vector>

#include "game/world_query.hpp"

namespace gtabot::nav {

using game::Vec3;

struct FieldResult {
  bool  ok = false;               // a route came out
  bool  reaches_target = false;   // and it ends at the target, not short of it
  float short_by_m = 0;           // when it does not: how far short
  std::vector<Vec3> points;       // from ... to, lifted to the ped origin
  float length_m = 0;
  std::string note;
  // How much world went into it.
  int   cells = 0, blocked = 0, unknown = 0, reached = 0, tiles = 0,
        ground_reads = 0, expanded = 0;
  int   took_ms = 0;
  // What went into it, for looking at when it goes wrong: the floor each
  // tile was painted against, and the ground read along the line from the
  // start toward the target, a metre a step (-1 where none was found).
  std::vector<float> tile_floors;
  std::vector<float> ground_line;
  float ref_z = 0;
  // The box, so a target or a way out that fell outside it can be seen to.
  float box_x0 = 0, box_y0 = 0, box_x1 = 0, box_y1 = 0;
  // The eight cells round where the route ended, when it ended short: for
  // each, whether it is passable, its ground and its clearance - the
  // question being why the search would not go on from there.
  std::vector<std::string> end_neighbours;
  int refused_shut = 0, refused_step = 0, refused_corner = 0;
  int ledges = 0;   // cells shut for standing at the lip of a drop
  float tallest_step = 0;
  // One character a cell, every other cell, rows from the north down.
  std::vector<std::string> picture;
};

class Field {
 public:
  Field();
  ~Field();
  Field(const Field&) = delete;
  Field& operator=(const Field&) = delete;

  // Both ends lifted to the ped origin, a metre over the ground.
  void Start(const Vec3& from, const Vec3& to);
  // One unit of work. True once there is nothing left to do.
  bool Step();
  bool finished() const;
  const FieldResult& result() const;
  // What a point of the finished field is: for looking at a cell the route
  // would not cross. Returns false outside the field or before it is done.
  struct CellInfo { bool passable = false, blocked = false; int known = 0; float ground = 0, clear = 0; };
  bool At(const Vec3& p, CellInfo* out) const;

 private:
  struct Work;
  Work* w_ = nullptr;
  FieldResult result_;
};

// In one go, for a tool that asked and is waiting. Bounded by time.
FieldResult PlanField(const Vec3& from, const Vec3& to, int deadline_ms);

}  // namespace gtabot::nav
