#pragma once
//
// Making the game hold the ground the planner needs to see.
//
// The game streams collision for the few hundred metres round the player and
// nothing else: further out the buildings are still in the world's lists but
// have no collision model, so a ray cast at them passes through and the field
// writes down "no floor here". That is why a plan can only see two hundred
// and forty metres, why a journey is walked in greedy stages, and why one of
// those stages can lead into a dead-end peninsula that eight hundred metres
// of walking has to be undone.
//
// The collision is the cheap part of the map - triangles and boxes, no
// textures - and a machine of today has room for all of it. So the planner
// asks for the areas its box covers, pinned in memory, and the game loads
// them the same way it loads them for the player.
//
#include <cstddef>

namespace gtabot::game::streaming {

// Whether the addresses this needs are where this build of the game keeps
// them. Everything below does nothing when it is false.
bool Ready();

// Asks for - and pins - the map collision covering the square, in world
// metres. Returns how many areas were newly asked for; zero means everything
// was already in memory, which is the usual answer after the first call.
// Loads synchronously: the first call over new ground costs a frame or two.
// Game thread only.
int PinCollisionOver(float x0, float y0, float x1, float y1);

// The streamer's own memory budget, in bytes. The game sets itself a small
// one - it was written for a console with 32 MB - and pinning collision the
// player does not need will run into it. Raised once, at start-up.
std::size_t MemoryBudget();
bool SetMemoryBudget(std::size_t bytes);

// How many areas are pinned so far, for the log.
int Pinned();

}  // namespace gtabot::game::streaming
