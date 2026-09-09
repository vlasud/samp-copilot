#pragma once
//
// The red cylinder on the ground, and the racing marker after it.
//
// A server marks where it wants somebody to go with a checkpoint, not with a
// pickup: the delivery point of a job, the next corner of a route, the spot
// to park. They are in none of the pools this module already reads - SA-MP
// keeps one of each in its own CGame, because a player can only ever be shown
// one at a time - so without them a character being sent somewhere by a job
// has no idea where.
//
// The layout is checked rather than trusted: the field after the two
// checkpoints in CGame is the cursor mode, at +0x55, and that offset was
// established here long ago for quite another reason. If the cursor mode
// still reads as a cursor mode, everything in front of it is where it is
// supposed to be.
//
#include <string>

#include "game/world_query.hpp"

namespace gtabot::samp {

using game::Vec3;

struct Checkpoint {
  bool  shown = false;
  Vec3  at;
  float size = 0;          // the cylinder's own size, as the server set it
  float away_m = 0;
};

struct RaceCheckpoint {
  bool  shown = false;
  Vec3  at;
  Vec3  next;              // where the one after this is, for a route
  float size = 0;
  int   type = -1;         // the server's own kind: ground, air, finish
  float away_m = 0;
};

// Reading only. `from` is where the character is, for the distance.
Checkpoint CheckpointNow(const Vec3& from);
RaceCheckpoint RaceCheckpointNow(const Vec3& from);

std::string CheckpointsNote();

}  // namespace gtabot::samp
