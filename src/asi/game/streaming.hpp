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

// Asks for - and pins - every collision area of the map there is, so that
// nothing the planner looks at is ever missing. This is the whole of San
// Andreas: a few hundred areas, boxes and triangles only, and no textures,
// which is why it fits at all. Costs one long load the first time and
// nothing after. Returns how many areas were newly asked for.
int PinWholeMap();

// Asks for - and pins - the path graph of the whole map: the sixty-four
// areas of nodes the game streams round the player exactly as it streams
// collision. Only five or six are ever loaded at once, which is why a
// corridor to a target half a mile off could not be built at all - the
// target's own area had no nodes in it to aim at. Sixty-four small files.
// Returns how many were newly asked for. Game thread only.
int PinPathNodes();

// Asks for - and pins - the map itself over a square: the sections of the
// world file whose buildings the game creates as the player comes near and
// destroys as he goes away.
//
// This is the one that matters. Collision is only the shape of a building;
// if the building itself has not been created there is nothing to take the
// shape of, and a ray cast at it passes through. Two hundred and seventy
// metres from where he stood the ground could not be read at all - not with
// every collision area in the map pinned - because at that distance there
// were no buildings in the game's pool to read. The planner asks for the
// ground under the target before it will plan anything, so a target further
// off than that was refused outright and the whole errand was walked on the
// whiskers. That is the wandering on long journeys.
//
// Returns how many sections were newly asked for. Game thread only.
int PinMapOver(float x0, float y0, float x1, float y1);

// How many sections are pinned, and how many the map has.
int MapSectionsPinned();

// How much collision is held, in bytes, as the streamer counts it.
std::size_t MemoryUsed();

// How many areas are pinned so far, for the log.
int Pinned();

}  // namespace gtabot::game::streaming
